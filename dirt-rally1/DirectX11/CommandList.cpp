// Include before util.h (or any header that includes util.h) to get pretty
// version of LockResourceCreationMode:
#include "lock.h"

#include "CommandList.h"

#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>

#include <algorithm>
#include <sstream>
#include "HackerDevice.h"
#include "HackerContext.h"
#include "Override.h"
#include "D3D11Wrapper.h"
#include "IniHandler.h"
#include "profiling.h"
#include "Hunting.h"
#include "cursor.h"

// TrueTriple DR1: atan2f/atanf para el aviso del ojo virtual. En DR2 llegaba
// por transitividad; explicito no molesta y no depende del orden de headers.
#include <math.h>

#include <D3DCompiler.h>

CustomResources customResources;
CustomShaders customShaders;
ExplicitCommandListSections explicitCommandListSections;
CommandListVariables command_list_globals;
std::vector<CommandListVariable*> persistent_variables;
std::vector<CommandList*> registered_command_lists;
std::unordered_set<CommandList*> command_lists_profiling;
std::unordered_set<CommandListCommand*> command_lists_cmd_profiling;
std::vector<std::shared_ptr<CommandList>> dynamically_allocated_command_lists;


// Adds consistent "3DMigoto" prefix to frame analysis log with appropriate
// level of indentation for the current recursion level. Using a
// macro instead of a function for this to concatenate static strings:
#define COMMAND_LIST_LOG(state, fmt, ...) \
	do { \
		(state)->mHackerContext->FrameAnalysisLog("3DMigoto%*s " fmt, state->recursion + state->extra_indent, "", __VA_ARGS__); \
	} while (0)

struct command_list_profiling_state {
	LARGE_INTEGER list_start_time;
	LARGE_INTEGER cmd_start_time;
	LARGE_INTEGER saved_recursive_time;
};

static inline void profile_command_list_start(CommandList *command_list, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	bool inserted;

	if ((Profiling::mode != Profiling::Mode::SUMMARY)
	 && (Profiling::mode != Profiling::Mode::TOP_COMMAND_LISTS))
		return;

	inserted = command_lists_profiling.insert(command_list).second;
	if (inserted) {
		command_list->time_spent_inclusive.QuadPart = 0;
		command_list->time_spent_exclusive.QuadPart = 0;
		command_list->executions = 0;
	}

	profiling_state->saved_recursive_time = state->profiling_time_recursive;
	state->profiling_time_recursive.QuadPart = 0;

	QueryPerformanceCounter(&profiling_state->list_start_time);
}

static inline void profile_command_list_end(CommandList *command_list, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	LARGE_INTEGER list_end_time, duration;

	if ((Profiling::mode != Profiling::Mode::SUMMARY)
	 && (Profiling::mode != Profiling::Mode::TOP_COMMAND_LISTS))
		return;

	QueryPerformanceCounter(&list_end_time);
	duration.QuadPart = list_end_time.QuadPart - profiling_state->list_start_time.QuadPart;
	command_list->time_spent_inclusive.QuadPart += duration.QuadPart;
	command_list->time_spent_exclusive.QuadPart += duration.QuadPart - state->profiling_time_recursive.QuadPart;
	command_list->executions++;
	state->profiling_time_recursive.QuadPart = profiling_state->saved_recursive_time.QuadPart + duration.QuadPart;
}

static inline void profile_command_list_cmd_start(CommandListCommand *cmd,
		command_list_profiling_state *profiling_state)
{
	bool inserted;

	if (Profiling::mode != Profiling::Mode::TOP_COMMANDS)
		return;

	inserted = command_lists_cmd_profiling.insert(cmd).second;
	if (inserted) {
		cmd->pre_time_spent.QuadPart = 0;
		cmd->post_time_spent.QuadPart = 0;
		cmd->pre_executions = 0;
		cmd->post_executions = 0;
	}

	QueryPerformanceCounter(&profiling_state->cmd_start_time);
}

static inline void profile_command_list_cmd_end(CommandListCommand *cmd, CommandListState *state,
		command_list_profiling_state *profiling_state)
{
	LARGE_INTEGER end_time;

	if (Profiling::mode != Profiling::Mode::TOP_COMMANDS)
		return;

	QueryPerformanceCounter(&end_time);
	if (state->post) {
		cmd->post_time_spent.QuadPart += end_time.QuadPart - profiling_state->cmd_start_time.QuadPart;
		cmd->post_executions++;
	} else {
		cmd->pre_time_spent.QuadPart += end_time.QuadPart - profiling_state->cmd_start_time.QuadPart;
		cmd->pre_executions++;
	}
}

static void _RunCommandList(CommandList *command_list, CommandListState *state, bool recursive=true)
{
	CommandList::Commands::iterator i;
	command_list_profiling_state profiling_state;

	if (state->recursion > MAX_COMMAND_LIST_RECURSION) {
		LogOverlay(LOG_WARNING, "WARNING: Command list recursion limit exceeded! Circular reference?\n");
		return;
	}

	if (command_list->commands.empty())
		return;

	if (recursive) {
		COMMAND_LIST_LOG(state, "%s {\n", state->post ? "post" : "pre");
		state->recursion++;
	}

	profile_command_list_start(command_list, state, &profiling_state);

	for (i = command_list->commands.begin(); i < command_list->commands.end() && !state->aborted; i++) {
		profile_command_list_cmd_start(i->get(), &profiling_state);
		(*i)->run(state);
		profile_command_list_cmd_end(i->get(), state, &profiling_state);
	}

	profile_command_list_end(command_list, state, &profiling_state);

	if (recursive) {
		state->recursion--;
		COMMAND_LIST_LOG(state, "}\n");
	}
}

static void CommandListFlushState(CommandListState *state)
{
	D3D11_MAPPED_SUBRESOURCE mappedResource;
	HRESULT hr;

	if (state->update_params && state->mHackerDevice->mIniTexture) {
		hr = state->mOrigContext1->Map(state->mHackerDevice->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (FAILED(hr)) {
			LogInfo("CommandListFlushState: Map failed\n");
			return;
		}
		memcpy(mappedResource.pData, G->iniParams.data(), sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
		state->mOrigContext1->Unmap(state->mHackerDevice->mIniTexture, 0);
		state->update_params = false;
		Profiling::iniparams_updates++;
	}
}

static void RunCommandListComplete(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		DrawCallInfo *call_info,
		ID3D11Resource **resource,
		ID3D11View *view,
		bool post)
{
	CommandListState state;
	state.mHackerDevice = mHackerDevice;
	state.mHackerContext = mHackerContext;
	state.mOrigDevice1 = mHackerDevice->GetPassThroughOrigDevice1();
	state.mOrigContext1 = mHackerContext->GetPassThroughOrigContext1();

	state.call_info = call_info;
	state.resource = resource;
	state.view = view;
	state.post = post;

	_RunCommandList(command_list, &state);
	CommandListFlushState(&state);
}

void RunCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		DrawCallInfo *call_info,
		bool post)
{
	ID3D11Resource **resource = NULL;
	if (call_info)
		resource = (ID3D11Resource**)call_info->indirect_buffer;

	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			call_info, resource, NULL, post);
}

void RunResourceCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		ID3D11Resource **resource,
		bool post)
{
	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			NULL, resource, NULL, post);
}

void RunViewCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list,
		ID3D11View *view,
		bool post)
{
	ID3D11Resource *res = NULL;

	if (view)
		view->GetResource(&res);

	RunCommandListComplete(mHackerDevice, mHackerContext, command_list,
			NULL, &res, view, post);

	if (res)
		res->Release();
}

void optimise_command_lists(HackerDevice *device)
{
	bool making_progress;
	bool ignore_cto_pre, ignore_cto_post;
	size_t i;
	CommandList::Commands::iterator new_end;
	DWORD start;

	LogInfo("Optimising command lists...\n");
	start = GetTickCount();

	for (CommandList *command_list : registered_command_lists) {
		for (i = 0; i < command_list->commands.size(); i++)
			command_list->commands[i]->optimise(device);
	}

	do {
		making_progress = false;
		ignore_cto_pre = true;
		ignore_cto_post = true;

		// If all TextureOverride sections have empty command lists of
		// either pre or post, we can treat checktextureoverride as a
		// noop. This is intended to catch the case where we only have
		// "pre" commands in the TextureOverride sections to optimise
		// out the implicit "post checktextureoverride" commands, as
		// these can add up if they are used on very common shaders
		// (e.g. in DOAXVV this can easily save 0.2fps on a 4GHz CPU,
		// more on a slower CPU since this command is used in the
		// shadow map shaders)
		//
		// FIXME: This should itself ignore any checktextureoverrides
		// inside these command lists
		for (auto &tolkv : G->mTextureOverrideMap) {
			for (TextureOverride &to : tolkv.second) {
				ignore_cto_pre = ignore_cto_pre && to.command_list.commands.empty();
				ignore_cto_post = ignore_cto_post && to.post_command_list.commands.empty();
			}
		}
		for (auto &tof : G->mFuzzyTextureOverrides) {
			ignore_cto_pre = ignore_cto_pre && tof->texture_override->command_list.commands.empty();
			ignore_cto_post = ignore_cto_post && tof->texture_override->post_command_list.commands.empty();
		}

		// Go through each registered command list and remove any
		// commands that are noops to eliminate the runtime overhead of
		// processing these
		for (CommandList *command_list : registered_command_lists) {
			for (i = 0; i < command_list->commands.size(); ) {
				if (command_list->commands[i]->noop(command_list->post, ignore_cto_pre, ignore_cto_post)) {
					LogInfo("Optimised out %s %S\n",
							command_list->post ? "post" : "pre",
							command_list->commands[i]->ini_line.c_str());
					command_list->commands.erase(command_list->commands.begin() + i);
					making_progress = true;
					continue;
				}
				i++;
			}
		}

		// TODO: Merge adjacent commands if possible, e.g. all the
		// commands in BuiltInCommandListUnbindAllRenderTargets would
		// be good candidates to merge into a single command. We could
		// add a special command for that particular case, but would be
		// nice if this sort of thing worked more generally.
	} while (making_progress);

	Profiling::update_cto_warning(!ignore_cto_post);

	LogInfo("Command List Optimiser finished after %ums\n", GetTickCount() - start);
	registered_command_lists.clear();
	dynamically_allocated_command_lists.clear();
}

static bool AddCommandToList(CommandListCommand *command,
		CommandList *explicit_command_list,
		CommandList *sensible_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wchar_t *section,
		const wchar_t *key, wstring *val)
{
	if (section && key) {
		command->ini_line = L"[" + wstring(section) + L"] " + wstring(key);
		if (val)
			command->ini_line += L" = " + *val;
	}

	if (explicit_command_list) {
		// User explicitly specified "pre" or "post", so only add the
		// command to that list
		explicit_command_list->commands.push_back(std::shared_ptr<CommandListCommand>(command));
	} else if (sensible_command_list) {
		// User did not specify which command list to add it to, but
		// the command they specified has a sensible default, so add it
		// to that list:
		sensible_command_list->commands.push_back(std::shared_ptr<CommandListCommand>(command));
	} else {
		// The command's default is to add it to both lists (e.g. the
		// checktextureoverride directive will call command lists in
		// another ini section with both pre and post lists, so the
		// principal of least unexpected behaviour says we add it to
		// both so that both those command lists will be called)
		//
		// Using a std::shared_ptr here to handle adding the same
		// pointer to two lists and have it garbage collected only once
		// both are destroyed:
		std::shared_ptr<CommandListCommand> p(command);
		pre_command_list->commands.push_back(p);
		if (post_command_list)
			post_command_list->commands.push_back(p);
	}

	return true;
}

static bool ParseCheckTextureOverride(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	int ret;

	CheckTextureOverrideCommand *operation = new CheckTextureOverrideCommand();

	// Parse value as consistent with texture filtering and resource copying
	ret = operation->target.ParseTarget(val->c_str(), true, ini_namespace);
	if (ret) {
		// If the user indicated an explicit command list we will run the pre
		// and post lists of the target list together.
		if (explicit_command_list)
			operation->run_pre_and_post_together = true;
		else if (post_command_list)
			G->implicit_post_checktextureoverride_used = true;

		return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);
	}

	delete operation;
	return false;
}

static bool ParseResetPerFrameLimits(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	CustomResources::iterator res;
	CustomShaders::iterator shader;
	wstring namespaced_section;

	ResetPerFrameLimitsCommand *operation = new ResetPerFrameLimitsCommand();

	if (!wcsncmp(val->c_str(), L"resource", 8)) {
		wstring resource_id(val->c_str());

		res = customResources.end();
		if (get_namespaced_section_name_lower(&resource_id, ini_namespace, &namespaced_section))
			res = customResources.find(namespaced_section);
		if (res == customResources.end())
			res = customResources.find(resource_id);
		if (res == customResources.end())
			goto bail;

		operation->resource = &res->second;
	}

	if (!wcsncmp(val->c_str(), L"customshader", 12) || !wcsncmp(val->c_str(), L"builtincustomshader", 19)) {
		wstring shader_id(val->c_str());

		shader = customShaders.end();
		if (get_namespaced_section_name_lower(&shader_id, ini_namespace, &namespaced_section))
			shader = customShaders.find(namespaced_section);
		if (shader == customShaders.end())
			shader = customShaders.find(shader_id);
		if (shader == customShaders.end())
			goto bail;

		operation->shader = &shader->second;
	}

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseClearView(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	CustomResources::iterator res;
	CustomShaders::iterator shader;
	wistringstream token_stream(*val);
	wstring token;
	int ret, len1;
	int idx = 0;
	unsigned uval;
	float fval;

	ClearViewCommand *operation = new ClearViewCommand();

	while (getline(token_stream, token, L' ')) {
		if (operation->target.type == ResourceCopyTargetType::INVALID) {
			ret = operation->target.ParseTarget(token.c_str(), true, ini_namespace);
			if (ret)
				continue;
		}

		if (idx < 4) {
			// Try parsing value as a hex string. If this matches
			// we know the user didn't intend to use floats. This
			// is necessary to allow integer values that require
			// more than 24 significant bits to be used, which
			// would be lost if we only parsed the string as a
			// float, e.g. 0xffffffff cannot be stored as a float
			ret = swscanf_s(token.c_str(), L"0x%x%n", &uval, &len1);
			if (ret != 0 && ret != EOF && len1 == token.length()) {
				operation->uval[idx] = uval;
				operation->fval[idx] = *(float*)&uval;
				operation->clear_uav_uint = true;
				idx++;
				continue;
			}

			// On the other hand, if parsing the value as a float
			// matches the user might have intended it to be a
			// float or an integer. We will assume they want floats
			// by default, but store it in both arrays in case we
			// later determine that we need to use an integer clear.
			ret = swscanf_s(token.c_str(), L"%f%n", &fval, &len1);
			if (ret != 0 && ret != EOF && len1 == token.length()) {
				operation->fval[idx] = fval;
				operation->uval[idx] = (UINT)fval;
				idx++;
				continue;
			}
		}
		if (!wcscmp(token.c_str(), L"int")) {
			operation->clear_uav_uint = true;
			continue;
		}
		if (!wcscmp(token.c_str(), L"depth")) {
			operation->clear_depth = true;
			continue;
		}
		if (!wcscmp(token.c_str(), L"stencil")) {
			operation->clear_stencil = true;
			continue;
		}

		goto bail;
	}

	if (operation->target.type == ResourceCopyTargetType::INVALID)
		goto bail;

	// Use the first value specified as the depth value when clearing a
	// DSV, and the second as the stencil value, unless we are only
	// clearing the stencil side, in which case use the first:
	operation->dsv_depth = operation->fval[0];
	operation->dsv_stencil = operation->uval[1];
	if (operation->clear_stencil && !operation->clear_depth)
		operation->dsv_stencil = operation->uval[0];

	// Propagate the final specified value to the remaining channels. This
	// allows a single value to be specified to clear all channels in RTVs
	// and UAVs. Note that this is done after noting the DSV values because
	// we never want to propagate the depth value to the stencil value:
	for (idx = max(1, idx); idx < 4; idx++) {
		operation->uval[idx] = operation->uval[idx - 1];
		operation->fval[idx] = operation->fval[idx - 1];
	}

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}


static bool ParseRunShader(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	RunCustomShaderCommand *operation = new RunCustomShaderCommand();
	CustomShaders::iterator shader;
	wstring namespaced_section;

	// Value should already have been transformed to lower case from
	// ParseCommandList, so our keys will be consistent in the
	// unordered_map:
	wstring shader_id(val->c_str());

	shader = customShaders.end();
	if (get_namespaced_section_name_lower(&shader_id, ini_namespace, &namespaced_section))
		shader = customShaders.find(namespaced_section);
	if (shader == customShaders.end())
		shader = customShaders.find(shader_id);
	if (shader == customShaders.end())
		goto bail;

	operation->custom_shader = &shader->second;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

bool ParseRunExplicitCommandList(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	RunExplicitCommandList *operation = new RunExplicitCommandList();
	ExplicitCommandListSections::iterator shader;
	wstring namespaced_section;

	// We need value in lower case so our keys will be consistent in the
	// unordered_map. ParseCommandList will have already done this, but the
	// Key/Preset parsing code will not have, and rather than require it to
	// we do it here:
	wstring section_id(val->c_str());
	std::transform(section_id.begin(), section_id.end(), section_id.begin(), ::towlower);

	shader = explicitCommandListSections.end();
	if (get_namespaced_section_name_lower(&section_id, ini_namespace, &namespaced_section))
		shader = explicitCommandListSections.find(namespaced_section);
	if (shader == explicitCommandListSections.end())
		shader = explicitCommandListSections.find(section_id);
	if (shader == explicitCommandListSections.end())
		goto bail;

	// If the user indicated an explicit command list we will run the pre
	// and post lists of the target list together. This tends to make
	// things a little less surprising for "post run = CommandListFoo"
	if (explicit_command_list)
		operation->run_pre_and_post_together = true;

	operation->command_list_section = &shader->second;
	// This function is nearly identical to ParseRunShader, but in case we
	// later refactor these together note that here we do not specify a
	// sensible command list, so it will be added to both pre and post
	// command lists:
	return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParsePreset(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		bool exclude, const wstring *ini_namespace)
{
	PresetCommand *operation = new PresetCommand();
	wstring prefixed_section, namespaced_section;

	PresetOverrideMap::iterator i;

	// Value should already have been transformed to lower case from
	// ParseCommandList, so our keys will be consistent in the
	// unordered_map:
	wstring preset_id(val->c_str());

	// The original preset code did not accept the "Preset" prefix on the
	// prefix command, as in it would only accept 'preset = Foo', not
	// 'preset = PresetFoo'. While I agree that the later is redundant
	// since the word preset now appears twice, it is more consistent with
	// the way we have referenced other sections in the command list (ps-t0
	// = ResourceBar, run = CustomShaderBaz, etc), and it makes it easier
	// to search for 'PresetFoo' to find both where it is used and where it
	// is referenced, so it is good to support here... but for backwards
	// compatibility and less redundancy for those that prefer not to say
	// "preset" twice we support both ways.
	prefixed_section = wstring(L"preset") + preset_id;

	// And now with namespacing, we have four permutations to try...
	i = presetOverrides.end();
	// First, add the 'Preset' (i.e. the user did not) and try namespaced:
	if (get_namespaced_section_name_lower(&prefixed_section, ini_namespace, &namespaced_section))
		i = presetOverrides.find(namespaced_section);
	// Second, try namespaced without adding the prefix:
	if (i == presetOverrides.end()) {
		if (get_namespaced_section_name_lower(&preset_id, ini_namespace, &namespaced_section))
			i = presetOverrides.find(namespaced_section);
	}
	// Third, add the 'Preset' and try global:
	if (i == presetOverrides.end())
		i = presetOverrides.find(prefixed_section);
	// Finally, don't add the prefix and try global:
	if (i == presetOverrides.end())
		i = presetOverrides.find(preset_id);
	if (i == presetOverrides.end())
		goto bail;

	operation->preset = &i->second;
	operation->exclude = exclude;

	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseDrawCommandArgs(wstring *val, DrawCommand *operation, bool indirect, int nargs, const wstring *ini_namespace, CommandListScope *scope)
{
	size_t start = 0, end;
	wstring sub;
	int i;

	if (indirect) {
		end = val->find(L',', start);
		if (end == wstring::npos)
			return false;

		sub = val->substr(start, end);
		if (!operation->indirect_buffer.ParseTarget(sub.c_str(), true, ini_namespace))
			return false;

		if (operation->indirect_buffer.type == ResourceCopyTargetType::CUSTOM_RESOURCE) {
			// Fucking C++ making this line 3x longer than it should be:
			operation->indirect_buffer.custom_resource->misc_flags = (D3D11_RESOURCE_MISC_FLAG)
				(operation->indirect_buffer.custom_resource->misc_flags
				 | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
		}

		start = end + 1;
	}

	for (i = 0; i < nargs; i++) {
		end = val->find(L',', start);

		sub = val->substr(start, end - start);
		if (!operation->args[i].parse(&sub, ini_namespace, scope))
			return false;

		if (end == wstring::npos)
			return (i+1 == nargs);
		start = end + 1;
	}

	return false;
}

static bool ParseDrawCommand(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	DrawCommand *operation = new DrawCommand();
	bool ok = true;

	if (!wcscmp(key, L"draw")) {
		if (!wcscmp(val->c_str(), L"from_caller")) {
			operation->type = DrawCommandType::FROM_CALLER;
		} else if (!wcscmp(val->c_str(), L"auto")) {
			operation->type = DrawCommandType::AUTO_VERTEX_COUNT;
		} else {
			operation->type = DrawCommandType::DRAW;
			ok = ParseDrawCommandArgs(val, operation, false, 2, ini_namespace, pre_command_list->scope);
		}
	} else if (!wcscmp(key, L"drawauto")) {
		operation->type = DrawCommandType::DRAW_AUTO;
	} else if (!wcscmp(key, L"drawindexed")) {
		if (!wcscmp(val->c_str(), L"auto")) {
			operation->type = DrawCommandType::AUTO_INDEX_COUNT;
		} else {
			operation->type = DrawCommandType::DRAW_INDEXED;
			ok = ParseDrawCommandArgs(val, operation, false, 3, ini_namespace, pre_command_list->scope);
		}
	} else if (!wcscmp(key, L"drawindexedinstanced")) {
		if (!wcscmp(val->c_str(), L"auto")) {
			operation->type = DrawCommandType::AUTO_INDEX_INSTANCE_COUNT;
		}
		else {
			operation->type = DrawCommandType::DRAW_INDEXED_INSTANCED;
			ok = ParseDrawCommandArgs(val, operation, false, 5, ini_namespace, pre_command_list->scope);
		}
	} else if (!wcscmp(key, L"drawinstanced")) {
		operation->type = DrawCommandType::DRAW_INSTANCED;
		ok = ParseDrawCommandArgs(val, operation, false, 4, ini_namespace, pre_command_list->scope);
	} else if (!wcscmp(key, L"dispatch")) {
		operation->type = DrawCommandType::DISPATCH;
		ok = ParseDrawCommandArgs(val, operation, false, 3, ini_namespace, pre_command_list->scope);
	} else if (!wcscmp(key, L"drawindexedinstancedindirect")) {
		operation->type = DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT;
		ok = ParseDrawCommandArgs(val, operation, true, 1, ini_namespace, pre_command_list->scope);
	} else if (!wcscmp(key, L"drawinstancedindirect")) {
		operation->type = DrawCommandType::DRAW_INSTANCED_INDIRECT;
		ok = ParseDrawCommandArgs(val, operation, true, 1, ini_namespace, pre_command_list->scope);
	} else if (!wcscmp(key, L"dispatchindirect")) {
		operation->type = DrawCommandType::DISPATCH_INDIRECT;
		ok = ParseDrawCommandArgs(val, operation, true, 1, ini_namespace, pre_command_list->scope);
	}

	if (operation->type == DrawCommandType::INVALID || !ok)
		goto bail;

	operation->ini_section = section;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseDirectModeSetActiveEyeCommand(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list)
{
	DirectModeSetActiveEyeCommand *operation = new DirectModeSetActiveEyeCommand();

	if (!wcscmp(val->c_str(), L"mono")) {
		operation->eye = NVAPI_STEREO_EYE_MONO;
		goto success;
	}

	if (!wcscmp(val->c_str(), L"left")) {
		operation->eye = NVAPI_STEREO_EYE_LEFT;
		goto success;
	}

	if (!wcscmp(val->c_str(), L"right")) {
		operation->eye = NVAPI_STEREO_EYE_RIGHT;
		goto success;
	}

	goto bail;

success:
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParsePerDrawStereoOverride(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		bool is_separation,
		const wstring *ini_namespace)
{
	bool restore_on_post = !explicit_command_list && pre_command_list && post_command_list;
	PerDrawStereoOverrideCommand *operation = NULL;

	if (is_separation)
		operation = new PerDrawSeparationOverrideCommand(restore_on_post);
	else
		operation = new PerDrawConvergenceOverrideCommand(restore_on_post);

	// Try parsing value as a resource target for staging auto-convergence
	// Do this first, because the operand parsing would treat these as for
	// texture filtering
	if (operation->staging_op.src.ParseTarget(val->c_str(), true, ini_namespace)) {
		operation->staging_type = true;
		goto success;
	}

	// The scope is shared between pre & post, we use pre here since it is never NULL
	if (!operation->expression.parse(val, ini_namespace, pre_command_list->scope))
		goto bail;

success:
	// Add to both command lists by default - the pre command list will set
	// the value, and the post command list will restore the original. If
	// an explicit command list is specified then the value will only be
	// set, not restored (regardless of whether that is pre or post)
	return AddCommandToList(operation, explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);

bail:
	delete operation;
	return false;
}

static bool ParseFrameAnalysisDump(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace)
{
	FrameAnalysisDumpCommand *operation = new FrameAnalysisDumpCommand();
	wchar_t *buf;
	size_t size = val->size() + 1;
	wchar_t *target = NULL;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	buf = new wchar_t[size];
	wcscpy_s(buf, size, val->c_str());

	operation->analyse_options = parse_enum_option_string<wchar_t *, FrameAnalysisOptions>
		(FrameAnalysisOptionNames, buf, &target);

	if (!target)
		goto bail;

	if (!operation->target.ParseTarget(target, true, ini_namespace))
		goto bail;

	operation->target_name = L"[" + wstring(section) + L"]-" + wstring(target);
	// target_name will be used in the filenames, so replace any reserved characters:
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'<', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'>', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L':', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'"', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'/', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'\\',L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'|', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'?', L'_');
	std::replace(operation->target_name.begin(), operation->target_name.end(), L'*', L'_');

	delete [] buf;
	return AddCommandToList(operation, explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

bail:
	delete [] buf;
	delete operation;
	return false;
}

bool ParseCommandListGeneralCommands(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	if (!wcscmp(key, L"checktextureoverride"))
		return ParseCheckTextureOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"run")) {
		if (!wcsncmp(val->c_str(), L"customshader", 12) || !wcsncmp(val->c_str(), L"builtincustomshader", 19))
			return ParseRunShader(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

		if (!wcsncmp(val->c_str(), L"commandlist", 11) || !wcsncmp(val->c_str(), L"builtincommandlist", 18))
			return ParseRunExplicitCommandList(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);
	}

	if (!wcscmp(key, L"preset"))
		return ParsePreset(section, key, val, explicit_command_list, pre_command_list, post_command_list, false, ini_namespace);
	if (!wcscmp(key, L"exclude_preset"))
		return ParsePreset(section, key, val, explicit_command_list, pre_command_list, post_command_list, true, ini_namespace);

	if (!wcscmp(key, L"handling")) {
		// skip only makes sense in pre command lists, since it needs
		// to run before the original draw call:
		if (!wcscmp(val->c_str(), L"skip"))
			return AddCommandToList(new SkipCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

		// abort defaults to both command lists, to abort command list
		// execution both before and after the draw call:
		if (!wcscmp(val->c_str(), L"abort"))
			return AddCommandToList(new AbortCommand(section), explicit_command_list, NULL, pre_command_list, post_command_list, section, key, val);
	}

	if (!wcscmp(key, L"reset_per_frame_limits"))
		return ParseResetPerFrameLimits(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	// ===== TrueTriple DR1: comandos nuevos del .ini ==========================
	// set_scissor = l, t, r, b | save | restore
	if (!wcscmp(key, L"set_scissor")) {
		if (!wcscmp(val->c_str(), L"save"))
			return AddCommandToList(new ScissorCommand(section, 1), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		if (!wcscmp(val->c_str(), L"restore"))
			return AddCommandToList(new ScissorCommand(section, 2), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		int l, t, r, b;
		if (swscanf_s(val->c_str(), L"%d, %d, %d, %d", &l, &t, &r, &b) == 4)
			return AddCommandToList(new ScissorCommand(section, l, t, r, b), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		LogInfo("  WARNING: set_scissor requires 4 integers (l,t,r,b) or save/restore\n");
		return false;
	}

	// triple_params = 0|1|2 : bindea la textura de parametros PRE-ARMADA de esa
	// region (sustituye a "x = N", que costaba un Map de recurso GPU por redibujo).
	if (!wcscmp(key, L"triple_params")) {
		int r = _wtoi(val->c_str());
		// 3 = passthrough EXPLICITO: obligatorio al final de los redibujos.
		// Si la textura de region quedara enlazada, los pases offscreen que usan
		// los mismos shaders parcheados (p.ej. el RELLENO DEL SHADOW MAP) leerian
		// x = 0/1/2 y se renderizarian deformados por region.
		if (r >= 0 && r <= 3)
			return AddCommandToList(new TripleParamsCommand(section, r), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		LogInfo("  WARNING: triple_params requires 0, 1, 2 or 3\n");
	}

	// triple_scissor = 0|1|2|restore : fija SOLO el scissor del tercio, como
	// fraccion del render target real -> funciona a cualquier resolucion.
	if (!wcscmp(key, L"triple_scissor")) {
		// "restore" = reponer los rects que el JUEGO habia seteado (copia sombra
		// de HackerContext); si no habia ninguno, rect neutro de 16384.
		if (!wcscmp(val->c_str(), L"restore"))
			return AddCommandToList(new TripleRegionCommand(section, -1, 2), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		int r = _wtoi(val->c_str());
		if (r >= 0 && r <= 2)
			return AddCommandToList(new TripleRegionCommand(section, r, 1), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		LogInfo("  WARNING: triple_scissor requires 0, 1, 2 or restore\n");
	}

	if (!wcscmp(key, L"triple_region")) {
		int r = -1;
		if (swscanf_s(val->c_str(), L"%d", &r) == 1 && r >= 0 && r <= 2)
			return AddCommandToList(new TripleRegionCommand(section, r), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
		LogInfo("  WARNING: triple_region requires 0, 1 or 2\n");
		return false;
	}

	if (!wcscmp(key, L"triple_restore"))
		return AddCommandToList(new TripleRestoreCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
	// ===== fin TrueTriple DR1 ================================================

	if (!wcscmp(key, L"clear"))
		return ParseClearView(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"separation"))
		return ParsePerDrawStereoOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, true, ini_namespace);

	if (!wcscmp(key, L"convergence"))
		return ParsePerDrawStereoOverride(section, key, val, explicit_command_list, pre_command_list, post_command_list, false, ini_namespace);

	if (!wcscmp(key, L"direct_mode_eye"))
		return ParseDirectModeSetActiveEyeCommand(section, key, val, explicit_command_list, pre_command_list, post_command_list);

	if (!wcscmp(key, L"analyse_options"))
		return AddCommandToList(new FrameAnalysisChangeOptionsCommand(val), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

	if (!wcscmp(key, L"dump"))
		return ParseFrameAnalysisDump(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);

	if (!wcscmp(key, L"special")) {
		if (!wcscmp(val->c_str(), L"upscaling_switch_bb"))
			return AddCommandToList(new UpscalingFlipBBCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);

		if (!wcscmp(val->c_str(), L"draw_3dmigoto_overlay"))
			return AddCommandToList(new Draw3DMigotoOverlayCommand(section), explicit_command_list, pre_command_list, NULL, NULL, section, key, val);
	}

	return ParseDrawCommand(section, key, val, explicit_command_list, pre_command_list, post_command_list, ini_namespace);
}

void CheckTextureOverrideCommand::run(CommandListState *state)
{
	TextureOverrideMatches matches;
	ResourceCopyTarget *saved_this = NULL;
	bool saved_post;
	unsigned i;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	target.FindTextureOverrides(state, NULL, &matches);

	saved_this = state->this_target;
	state->this_target = &target;
	if (run_pre_and_post_together) {
		saved_post = state->post;
		state->post = false;
		for (i = 0; i < matches.size(); i++)
			_RunCommandList(&matches[i]->command_list, state);
		state->post = true;
		for (i = 0; i < matches.size(); i++)
			_RunCommandList(&matches[i]->post_command_list, state);
		state->post = saved_post;
	} else {
		for (i = 0; i < matches.size(); i++) {
			if (state->post)
				_RunCommandList(&matches[i]->post_command_list, state);
			else
				_RunCommandList(&matches[i]->command_list, state);
		}
	}
	state->this_target = saved_this;
}

bool CheckTextureOverrideCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	if (run_pre_and_post_together)
		return (ignore_cto_pre && ignore_cto_post);

	if (post)
		return ignore_cto_post;
	return ignore_cto_pre;
}

ClearViewCommand::ClearViewCommand() :
	dsv_depth(0.0),
	dsv_stencil(0),
	clear_depth(false),
	clear_stencil(false),
	clear_uav_uint(false)
{
	memset(fval, 0, sizeof(fval));
	memset(uval, 0, sizeof(uval));
}

static bool UAVSupportsFloatClear(ID3D11UnorderedAccessView *uav)
{
	D3D11_UNORDERED_ACCESS_VIEW_DESC desc;

	// UAVs can be cleared as a float if their format is a float, snorm or
	// unorm, otherwise the clear will fail silently. I didn't include
	// partially typed or block compressed formats in the below list
	// because I doubt they would work (but haven't checked).

	uav->GetDesc(&desc);

	switch (desc.Format) {
		case DXGI_FORMAT_UNKNOWN:
			// Common case
			return false;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_A8_UNORM:
		case DXGI_FORMAT_R1_UNORM:
		case DXGI_FORMAT_R8G8_B8G8_UNORM:
		case DXGI_FORMAT_G8R8_G8B8_UNORM:
		case DXGI_FORMAT_B5G6R5_UNORM:
		case DXGI_FORMAT_B5G5R5A1_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			return true;
		default:
			return false;
	}
}

void ResetPerFrameLimitsCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (shader)
		shader->executions_this_frame = 0;

	if (resource)
		resource->copies_this_frame = 0;
}

void PresetCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (exclude)
		preset->Exclude();
	else
		preset->Trigger(this);
}

static UINT get_vertex_count_from_current_vb(ID3D11DeviceContext* mOrigContext1)
{
	ID3D11Buffer* vb;
	D3D11_BUFFER_DESC desc;
	UINT stride;
	UINT offset;

	// Using vb slot 0. This is kind of making an assumption that vb0 is
	// assigned and has per-vertex (not per-instance) data, but that's probably
	// going to be a fairly safe bet in almost every game. We could go to more
	// heorics to check other vertex buffers if slot 0 isn't assigned, and to
	// check the input layout to make sure it holds per-vertex data, but it's
	// probably not worth it, especially as there's no API to get the input
	// layout description, and while we have stored it in private data for
	// frame analysis purposes, querying private data is a slow and not
	// something we want to do in the hot path of the render pipeline.
	//
	// Let's just keep this simple and fast, and if a game comes along that
	// doesn't work with this the modder can always specify the vertex count
	// manually.
	mOrigContext1->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
	if (!vb)
		return 0;

	vb->GetDesc(&desc);
	vb->Release();

	return (desc.ByteWidth - offset) / stride;
}

static UINT get_index_count_from_current_ib(ID3D11DeviceContext *mOrigContext1)
{
	ID3D11Buffer *ib;
	D3D11_BUFFER_DESC desc;
	DXGI_FORMAT format;
	UINT offset;

	mOrigContext1->IAGetIndexBuffer(&ib, &format, &offset);
	if (!ib)
		return 0;

	ib->GetDesc(&desc);
	ib->Release();

	switch(format) {
		case DXGI_FORMAT_R16_UINT:
			return (desc.ByteWidth - offset) / 2;
		case DXGI_FORMAT_R32_UINT:
			return (desc.ByteWidth - offset) / 4;
	}

	return 0;
}

void DrawCommand::do_indirect_draw_call(CommandListState *state, char *name,
		void (__stdcall ID3D11DeviceContext::*IndirectDrawCall)(THIS_
		ID3D11Buffer *pBufferForArgs,
		UINT AlignedByteOffsetForArgs))
{
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	UINT stride = 0;
	UINT offset = 0;
	UINT buf_size = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT arg = (UINT)args[0].evaluate(state);

	resource = indirect_buffer.GetResource(state, &view, &stride, &offset, &format, NULL);
	if (view)
		view->Release();

	if (!resource) {
		COMMAND_LIST_LOG(state, "[%S] %s(%p, %u) -> INDIRECT BUFFER IS NULL\n",
				ini_section.c_str(), name, resource, arg);
		return;
	}

	COMMAND_LIST_LOG(state, "[%S] %s(%p, %u)\n", ini_section.c_str(), name, resource, arg);

	(state->mOrigContext1->*IndirectDrawCall)((ID3D11Buffer*)resource, arg);

	resource->Release();
}

void DrawCommand::eval_args(int nargs, INT result[5], CommandListState *state)
{
	for (int i = 0; i < nargs; i++)
		result[i] = (INT)args[i].evaluate(state);
}

void DrawCommand::run(CommandListState *state)
{
	HackerContext *mHackerContext = state->mHackerContext;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	DrawCallInfo *info = state->call_info;
	UINT auto_count = 0;
	INT eargs[5];

	// If this command list was triggered from something currently skipped
	// due to hunting, we also skip any custom draw calls, so that if we
	// are replacing the original draw call we will still be able to see
	// the object being hunted.
	if (info && info->hunting_skip) {
		COMMAND_LIST_LOG(state, "[%S] Draw -> SKIPPED DUE TO HUNTING\n", ini_section.c_str());
		return;
	}

	// Ensure IniParams are visible:
	CommandListFlushState(state);

	Profiling::injected_draw_calls++;

	switch (type) {
		case DrawCommandType::DRAW:
			eval_args(2, eargs, state);
			COMMAND_LIST_LOG(state, "[%S] Draw(%u, %u)\n", ini_section.c_str(), eargs[0], eargs[1]);
			mOrigContext1->Draw(eargs[0], eargs[1]);
			break;
		case DrawCommandType::DRAW_AUTO:
			COMMAND_LIST_LOG(state, "[%S] DrawAuto()\n", ini_section.c_str());
			mOrigContext1->DrawAuto();
			break;
		case DrawCommandType::DRAW_INDEXED:
			eval_args(3, eargs, state);
			COMMAND_LIST_LOG(state, "[%S] DrawIndexed(%u, %u, %i)\n", ini_section.c_str(), eargs[0], eargs[1], (INT)eargs[2]);
			mOrigContext1->DrawIndexed(eargs[0], eargs[1], (INT)eargs[2]);
			break;
		case DrawCommandType::DRAW_INDEXED_INSTANCED:
			eval_args(5, eargs, state);
			COMMAND_LIST_LOG(state, "[%S] DrawIndexedInstanced(%u, %u, %u, %i, %u)\n", ini_section.c_str(), eargs[0], eargs[1], eargs[2], (INT)eargs[3], eargs[4]);
			mOrigContext1->DrawIndexedInstanced(eargs[0], eargs[1], eargs[2], (INT)eargs[3], eargs[4]);
			break;
		case DrawCommandType::DRAW_INSTANCED:
			eval_args(4, eargs, state);
			COMMAND_LIST_LOG(state, "[%S] DrawInstanced(%u, %u, %u, %u)\n", ini_section.c_str(), eargs[0], eargs[1], eargs[2], eargs[3]);
			mOrigContext1->DrawInstanced(eargs[0], eargs[1], eargs[2], eargs[3]);
			break;
		case DrawCommandType::DISPATCH:
			eval_args(3, eargs, state);
			COMMAND_LIST_LOG(state, "[%S] Dispatch(%u, %u, %u)\n", ini_section.c_str(), eargs[0], eargs[1], eargs[2]);
			mOrigContext1->Dispatch(eargs[0], eargs[1], eargs[2]);
			break;
		case DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT:
			do_indirect_draw_call(state, "DrawIndexedInstancedIndirect", &ID3D11DeviceContext::DrawIndexedInstancedIndirect);
			break;
		case DrawCommandType::DRAW_INSTANCED_INDIRECT:
			do_indirect_draw_call(state, "DrawInstancedIndirect", &ID3D11DeviceContext::DrawInstancedIndirect);
			break;
		case DrawCommandType::DISPATCH_INDIRECT:
			do_indirect_draw_call(state, "DispatchIndirect", &ID3D11DeviceContext::DispatchIndirect);
			break;
		case DrawCommandType::FROM_CALLER:
			if (!info) {
				COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> NO ACTIVE DRAW CALL\n", ini_section.c_str());
				break;
			}
			switch (info->type) {
				case DrawCall::DrawIndexedInstanced:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexedInstanced(%u, %u, %u, %i, %u)\n", ini_section.c_str(), info->IndexCount, info->InstanceCount, info->FirstIndex, info->FirstVertex, info->FirstInstance);
					mOrigContext1->DrawIndexedInstanced(info->IndexCount, info->InstanceCount, info->FirstIndex, info->FirstVertex, info->FirstInstance);
					break;
				case DrawCall::DrawInstanced:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawInstanced(%u, %u, %u, %u)\n", ini_section.c_str(), info->VertexCount, info->InstanceCount, info->FirstVertex, info->FirstInstance);
					mOrigContext1->DrawInstanced(info->VertexCount, info->InstanceCount, info->FirstVertex, info->FirstInstance);
					break;
				case DrawCall::DrawIndexed:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexed(%u, %u, %i)\n", ini_section.c_str(), info->IndexCount, info->FirstIndex, info->FirstVertex);
					mOrigContext1->DrawIndexed(info->IndexCount, info->FirstIndex, info->FirstVertex);
					break;
				case DrawCall::Draw:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> Draw(%u, %u)\n", ini_section.c_str(), info->VertexCount, info->FirstVertex);
					mOrigContext1->Draw(info->VertexCount, info->FirstVertex);
					break;
				case DrawCall::DrawInstancedIndirect:
					if (!info->indirect_buffer) {
						LogOverlay(LOG_DIRE, "BUG: draw = from_caller -> DrawInstancedIndirect missing args\n");
						break;
					}
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawInstancedIndirect(0x%p, %u)\n", ini_section.c_str(), *info->indirect_buffer, info->args_offset);
					mOrigContext1->DrawInstancedIndirect(*info->indirect_buffer, info->args_offset);
					break;
				case DrawCall::DrawIndexedInstancedIndirect:
					if (!info->indirect_buffer) {
						LogOverlay(LOG_DIRE, "BUG: draw = from_caller -> DrawIndexedInstancedIndirect missing args\n");
						break;
					}
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawIndexedInstancedIndirect(0x%p, %u)\n", ini_section.c_str(), *info->indirect_buffer, info->args_offset);
					mOrigContext1->DrawIndexedInstancedIndirect(*info->indirect_buffer, info->args_offset);
					break;
				case DrawCall::DispatchIndirect:
					if (!info->indirect_buffer) {
						LogOverlay(LOG_DIRE, "BUG: draw = from_caller -> DispatchIndirect missing args\n");
						break;
					}
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DispatchIndirect(0x%p, %u)\n", ini_section.c_str(), *info->indirect_buffer, info->args_offset);
					mOrigContext1->DispatchIndirect(*info->indirect_buffer, info->args_offset);
					break;
				case DrawCall::Dispatch:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> Dispatch(%u, %u, %u)\n", ini_section.c_str(), info->ThreadGroupCountX, info->ThreadGroupCountY, info->ThreadGroupCountZ);
					mOrigContext1->Dispatch(info->ThreadGroupCountX, info->ThreadGroupCountY, info->ThreadGroupCountZ);
					break;
				case DrawCall::DrawAuto:
					COMMAND_LIST_LOG(state, "[%S] Draw = from_caller -> DrawAuto()\n", ini_section.c_str());
					mOrigContext1->DrawAuto();
					break;
				default:
					LogOverlay(LOG_DIRE, "BUG: draw = from_caller -> unknown draw call type\n");
					break;
			}
			break;
		case DrawCommandType::AUTO_VERTEX_COUNT:
			auto_count = get_vertex_count_from_current_vb(mOrigContext1);
			COMMAND_LIST_LOG(state, "[%S] draw = auto -> Draw(%u, 0)\n", ini_section.c_str(), auto_count);
			if (auto_count)
				mOrigContext1->Draw(auto_count, 0);
			else
				COMMAND_LIST_LOG(state, "  Unable to determine vertex count\n");
			break;
		case DrawCommandType::AUTO_INDEX_COUNT:
			auto_count = get_index_count_from_current_ib(mOrigContext1);
			COMMAND_LIST_LOG(state, "[%S] drawindexed = auto -> DrawIndexed(%u, 0, 0)\n", ini_section.c_str(), auto_count);
			if (auto_count)
				mOrigContext1->DrawIndexed(auto_count, 0, 0);
			else
				COMMAND_LIST_LOG(state, "  Unable to determine index count\n");
			break;
		case DrawCommandType::AUTO_INDEX_INSTANCE_COUNT:
			if (!info) {
				COMMAND_LIST_LOG(state, "[%S] drawindexedinstanced = auto -> NO ACTIVE DRAW CALL\n", ini_section.c_str());
				break;
			}
			auto_count = get_index_count_from_current_ib(mOrigContext1);
			COMMAND_LIST_LOG(state, "[%S] drawindexedinstanced = auto -> DrawIndexedInstanced(%u, %u, 0, 0, %u)\n", ini_section.c_str(), auto_count, info->InstanceCount, info->FirstInstance);
			if (auto_count)
				mOrigContext1->DrawIndexedInstanced(auto_count, info->InstanceCount, 0, 0, info->FirstInstance);
			else
				COMMAND_LIST_LOG(state, "  Unable to determine index count\n");
			break;
	}
}

void SkipCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] handling = skip\n", ini_section.c_str());

	if (state->call_info)
		state->call_info->skip = true;
	else
		COMMAND_LIST_LOG(state, "  No active draw call to skip\n");
}

void AbortCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] handling = abort\n", ini_section.c_str());

	state->aborted = true;
}

// ============================================================================
// ===== TrueTriple DR1 - soporte de triple monitor real (3 frustums) ==========
// ============================================================================
//
// Port a DiRT Rally (2015) del mod TrueTriple de DiRT Rally 2.0.
//
// DIFERENCIA PRINCIPAL CON DR2: drt.exe es x86 (32 bits). Todo lo que aca
// dependia del ancho del puntero, del layout de CONTEXT de x64 o de offsets
// horneados del ejecutable de DR2 se quito o se corrigio. Cada cambio respecto
// de DR2 va marcado con "// TrueTriple DR1:".
//
// El ancla de la camara es la MISMA que en DR2 (verificado offline sobre
// shaderpack.pssg, ver docs/PSSG.md): CameraParamsConstantBuffer en el registro
// b3, 320 bytes, un solo layout, con
//     projection              cb3[0..3]   -> f[0..15]
//     inverseProj             cb3[4..7]   -> f[16..31]
//     eyePositionWS           cb3[8]      -> f[32..35]
//     relativeViewProjection  cb3[9..12]  -> f[36..51]
//     relativeView            cb3[13..15] -> f[52..63]
//     leftEye                 cb3[16]     -> f[64..67]
// Todas column-major: cada registro del cbuffer es una COLUMNA.
//
// LO QUE NO SE PORTO (a proposito, ver src-3dmigoto/PATCHES.md):
//   - CULLSCAN / CULLVITAL / CULLWIDEN (escaneo de los 6 planos del frustum en
//     RAM y reescritura en bucle). Diagnostico de un camino que en DR2 quedo
//     documentado como fallido; ademas asume un espacio de direcciones de 64
//     bits (barria ~8 GB) que en un proceso x86 no existe.
//   - BFHOOK (hook de BuildFrustum en <exe de DR2>+0x8B85D0, con verificacion
//     del prologo x64 "48 8B C4..."). Direccion y prologo son de DR2 y de x64:
//     no tienen equivalente en drt.exe. En DR2 quedo ABANDONADO por crashes.
//   - CULLBP (breakpoint de hardware DR0-DR3 leyendo CONTEXT.Rip/Rsp/Rax).
//     En Win32 el CONTEXT tiene Eip/Esp/Eax: ese codigo NO COMPILA en x86.
//   - TripleScanPlanesInCB / cb_snap_store / SOMBRAMED / GCSRV / CSCENSO:
//     mediciones ya neutralizadas en DR2 (v1.74) y ligadas a hashes de shaders
//     de DR2, que no sirven en DR1.

// Save/restore de los scissor rects propios del juego alrededor de nuestros
// redibujos por region. Sin esto, nuestro ultimo set_scissor se filtra al dibujo
// de la UI (que usa scissor para recortar paneles) y rompe los menus.
static thread_local D3D11_RECT tls_triple_saved_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
static thread_local UINT tls_triple_saved_rect_count = 0;

void ScissorCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] set_scissor mode=%d rect=%ld,%ld,%ld,%ld\n",
		ini_section.c_str(), mode, rect.left, rect.top, rect.right, rect.bottom);

	if (mode == 1) {
		// OJO (regla dura 3): RSGetScissorRects sobre un contexto diferido no es
		// fiable. Este modo "save" queda solo por compatibilidad con el ini; el
		// camino bueno es triple_scissor = restore, que usa la copia sombra que
		// HackerContext toma al pasar.
		tls_triple_saved_rect_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		state->mOrigContext1->RSGetScissorRects(&tls_triple_saved_rect_count, tls_triple_saved_rects);
	} else if (mode == 2) {
		state->mOrigContext1->RSSetScissorRects(tls_triple_saved_rect_count,
			tls_triple_saved_rect_count ? tls_triple_saved_rects : NULL);
	} else {
		state->mOrigContext1->RSSetScissorRects(1, &rect);
	}
}

// ---- Override de la camara por region --------------------------------------
// Las matrices de region NO se hornean: se construyen en RUNTIME desde
//   (a) las constantes de geometria K que el d3dx.ini publica en IniParams
//       ([Present] las recalcula por frame desde $eye_d / $panel / $angulo), y
//   (b) el near REAL de la camara capturada.
// Hornearlas fue un bug en DR2: la inversa tiene que usar el mismo near que
// preserva la correccion del shader, o las sombras quedan a profundidad
// equivocada.
//
// Layout de IniParams (ver [Present] en d3dx.ini):
//   fila0 = (region, KA, KB, KC)   fila1 = (KXc, KYc, sen_ang, cos_ang)
//
// AVISO PARA EL AGENTE DEL INI (regla dura 11): este C++ ESCRIBE cada frame
//   iniParams[3].x  (1 = camara de menu detectada -> el ini pasa a passthrough)
//   iniParams[3].w  (m11 de la camara POR LA QUE SE MIRA, no la ultima del frame)
//   iniParams[7].w  (near de ESA MISMA camara; con [3].w forman su firma, y de
//                    ahi sale si es una vista de conduccion -> ver el bloque de
//                    la firma mas abajo)
//   iniParams[8].x  (NUMERO DE PLANTEL del auto cargado, 1..N; 0 = ninguno.
//                    Lo publica tt_car_update() leyendo el id de modelo del motor)
//   iniParams[9].y  (seno del ROLIDO, v6; misma camara que el cabeceo)
//   iniParams[9].z  (ALTURA de la camara en el mundo, v6; para el resorte
//                    vertical del estabilizador)
//   iniParams[9].w  (seno del cabeceo, la señal del estabilizador de la mirada)
// y LEE iniParams[0].yzw, [1].xyzw, [3].y, [3].z, [6].zw, [7].xy.
// No usar esos componentes para otra cosa desde el ini.
//
// [7].w era el UNICO componente libre que quedaba, y lo dice la plantilla. Si
// hace falta otro, ya no hay: habra que agrandar iniParams.
#define TRIPLE_KA   (G->iniParams[0].y)
#define TRIPLE_KB   (G->iniParams[0].z)
#define TRIPLE_KC   (G->iniParams[0].w)
#define TRIPLE_KXC  (G->iniParams[1].x)
#define TRIPLE_KYC  (G->iniParams[1].y)
#define TRIPLE_SA   (G->iniParams[1].z)
#define TRIPLE_CA   (G->iniParams[1].w)

// TODO ESTO ES thread_local (uno por contexto diferido) - regla dura 5.
// CAUSA DEL PARPADEO en DR2 v1.14: eran globales compartidos. El juego graba en
// varios contextos diferidos, y en un contexto diferido UpdateSubresource NO se
// ejecuta al llamarlo: queda GRABADO en la lista de comandos de ESE contexto.
// Con un unico juego de buffers y un unico contador de frame:
//   - el hilo A veia frame nuevo, grababa el Update y marcaba el frame hecho;
//   - el hilo B veia el frame ya hecho, NO grababa nada, y bindeaba igual;
//   - si la lista de B se ejecutaba ANTES que la de A, B dibujaba con el
//     contenido del frame ANTERIOR -> valores alternando = parpadeo.
static thread_local ID3D11Buffer *g_triple_region_cb[3] = { NULL, NULL, NULL };
// Copia sin modificar de la camara capturada, para restaurar sin usar Get* (los
// metodos Get* de ID3D11DeviceContext no son fiables en contextos diferidos).
static thread_local ID3D11Buffer *g_triple_orig_cb = NULL;
static thread_local UINT g_triple_cb_size = 0;
static thread_local unsigned g_triple_frame = 0xFFFFFFFF;
static thread_local bool g_triple_valid = false;

// Camara capturada del lado CPU (desde Map/Unmap en HackerContext) para no
// depender de un readback de GPU, ilegal en contextos diferidos.
// TrueTriple DR1: alignas(16). En DR2 era un `char[512]` suelto y se castea a
// `const float *`. En x64 MSVC alinea los arrays grandes a 16 por su cuenta;
// en x86 la garantia es menor (alineacion natural del tipo = 1 para char), asi
// que la lectura como float podia quedar desalineada. En x86 eso no falla, pero
// es UB y penaliza; alignas lo vuelve correcto en las dos arquitecturas.
alignas(16) static char g_triple_cam_buf[512];
static UINT g_triple_cam_size = 0;
static volatile bool g_triple_cam_valid = false;

static CommandListVariable *cl_global(const wchar_t *nombre)
{
	auto it = command_list_globals.find(nombre);
	return (it != command_list_globals.end()) ? &it->second : NULL;
}

// ===== aviso en pantalla del ojo virtual =====================================
// Al ajustar el FOV a mano hace falta saber DONDE se esta parado: cuanto se ve,
// cuanto dibuja el juego, y si sobra (o sea si los bordes de los laterales
// quedan vacios). Sin esto, elegir el punto de compromiso es a ciegas.
// Va como LOG_WARNING y no como LOG_NOTICE a proposito: NOTICE tiene
// hide_in_release=true, o sea que fuera del modo hunting no se dibuja nunca.
static volatile LONG g_hud_state = 0;

static DWORD WINAPI eye_hud_thread(LPVOID)
{
	CommandListVariable *v_eye = cl_global(L"$eye_d");
	CommandListVariable *v_pw  = cl_global(L"$panel_w");
	CommandListVariable *v_sa  = cl_global(L"$side_sa");
	CommandListVariable *v_ca  = cl_global(L"$side_ca");
	if (!v_eye || !v_pw || !v_sa || !v_ca) {
		LogInfo("EYEHUD: faltan variables del ini; no se muestra el aviso.\n");
		return 0;
	}

	// TrueTriple DR1: EL CARTEL DICE UNA SOLA COSA -- si el mod esta prendido.
	// Pedido del autor, 2026-09-02: *"que solo diga TrueTriple ON o OFF"*.
	//
	// Antes decia tambien el ojo virtual, los grados que se ven y los que dibuja
	// el juego. Eso servia para elegir el FOV a mano; con el puesto ya calibrado
	// es ruido permanente encima de la imagen. El calculo NO se borro: sigue
	// entero unas lineas mas abajo y va al LOG, asi que el dato se recupera de
	// una corrida sin tener que volver a escribir la cuenta.
	//
	// Sin $triple_on no hay nada que mostrar: antes el cartel caia al modo
	// "solo ojo virtual", que es justo lo que se saco.
	CommandListVariable *v_on = cl_global(L"$triple_on");
	if (!v_on) {
		LogInfo("EYEHUD: el ini no declara $triple_on; no hay cartel que mostrar.\n");
		return 0;
	}

	float ultimo = -12345.0f;
	float ultimo_modo = -12345.0f;
	while (true) {
		float e = v_eye->fval;
		float modo = v_on->fval;

		// El cartel se redibuja SOLO cuando cambia el modo. Antes se disparaba
		// tambien al mover $eye_d, o sea que hacer zoom repetia el cartel sin que
		// hubiera pasado nada nuevo.
		if (modo != ultimo_modo) {
			ultimo_modo = modo;
			LogOverlay(LOG_WARNING, "TrueTriple %s\n",
				(modo <  0.5f) ? "OFF" :
				(modo <  1.5f) ? "ON" :
				                 "ON (diagnostico)");
		}

		// Lo de abajo ya NO se dibuja: queda como instrumento en el log.
		if (e != ultimo) {
			ultimo = e;
			// Esquina externa del panel lateral, vista desde arriba, con el
			// centro sobre el eje X y el ojo por delante.
			float pw = v_pw->fval, sa = v_sa->fval, ca = v_ca->fval;
			float xc = pw * 0.5f + pw * ca;
			float yc = pw * sa;
			// atan2 y no atan: con el ojo por delante de la esquina el angulo
			// pasa de 90 grados y hay que seguir midiendolo bien.
			float fov = 2.0f * atan2f(xc, e - yc) * 57.29578f;

			float m00 = g_triple_cam_valid ? ((const float *)g_triple_cam_buf)[0] : 0.0f;
			float fovj = (m00 > 0.0001f) ? 2.0f * atanf(1.0f / m00) * 57.29578f : 0.0f;

			if (fovj > 1.0f) {
				float sobra = fov - fovj;
				LogInfo("EYEHUD: ojo virtual %.0f cm | ves %.0f deg | "
					"el juego dibuja %.0f deg | %s\n", e, fov, fovj,
					(sobra > 1.0f)
						? "sobran los bordes de los laterales"
						: "sin bordes vacios");
			} else {
				LogInfo("EYEHUD: ojo virtual %.0f cm | ves %.0f deg\n", e, fov);
			}
		}
		Sleep(120);
	}
}

// ===========================================================================
// IDENTIFICACION DEL AUTO POR SU MALLA (port del v1.36 de DR2)
//
// PARA QUE: para poder guardar una postura de asiento POR AUTO. Nada de lo que
// ya leemos distingue el auto: medido en DR2, las camaras de DiRT tienen los
// mismos parametros en todos los autos (se probo con dos autos y no aparecio ni
// una firma de camara nueva). Hace falta una huella que salga del auto mismo.
//
// POR QUE LA MALLA Y NO UNA TEXTURA: 3Dmigoto hashea los recursos por
// CONTENIDO, asi que la textura del auto serviria... hasta que el usuario le
// cambia la PINTURA, y ahi el mismo auto pasa a parecer otro. Lo hizo notar el
// propio usuario de DR2, verificado en el juego con un auto repintado. La
// geometria del interior, en cambio, es la misma pintes como pintes.
//
// POR QUE LA MALLA MAS GRANDE Y NO EL CONJUNTO: el conjunto de piezas del
// habitaculo cambia con el culling (el volante, los relojes, las manijas
// entran y salen), asi que un hash del conjunto seria inestable frame a frame.
// La pieza principal -la carroceria del habitaculo- se dibuja siempre.
//
// ---------------------------------------------------------------------------
// LA DIFERENCIA CON DR2: EN DR1 NO HAY QUE CAZAR EL SHADER
//
// En DR2 el shader del interior (d0d1ddd09d064e71) salio de una sesion de caza
// con el usuario: se salteaba un shader, se miraba si el habitaculo se ponia
// negro. En DR1 el shaderpack trae los NOMBRES de Codemasters, asi que la
// eleccion se hace offline y con evidencia (analisis/shaderpack/indice.json,
// 4034 entradas):
//
//   hash 3Dmigoto     entradas del pack que lo usan          ajenas al interior
//   3a5942ef27e89153  31  *.interior.cg.vp_basic             0
//   e216301f2873cb1f  12  *.interior.cg.vp_basic_scroll      0
//   60a2ed2e5eb3d0e6   6  *.interior.cg.vp_full              0
//   b29da47f8ae09aac   5  *.interior.cg.vp                   0
//   13a53fb389fa2c87   2  interior_carbon.interior_carbon.cg.vp  0
//
// Las 56 entradas son materiales de habitaculo (int_alpha_di, int_rim_di_nm_*,
// interior_flock, interior_nozclear, interior_carbon...) y NINGUNA otra entrada
// del pack comparte esos cinco bytecodes: son EXCLUSIVOS del interior del auto.
// Ademas los cinco CORREN de verdad: aparecen en los dos logs reales del
// proyecto (analisis/prueba01 y analisis/prueba03) con
// "Performing deferred shader analysis on vs <hash>" y
// "ShaderRegex: vs_5_0 3a5942ef27e89153 matches [ShaderRegexTripleGlobalsA]"
// (regla dura 10: solo se conecta algo con evidencia de que el pase se ejecuta).
//
// POR QUE CINCO Y NO UNO, que es lo que hacia DR2: el material del cuerpo del
// habitaculo lo elige el ARTISTA de cada auto. Un auto puede tener la carroceria
// interior en un material que compila a vp_basic y otro en uno que compila a
// vp_full; con un solo hash, el segundo auto publicaria la malla de otra pieza o
// ninguna. Se toma el maximo sobre la UNION de los cinco: la malla mas grande
// del habitaculo es la misma sin importar por cual de los cinco entre.
//
// QUE SE DEJO AFUERA Y POR QUE:
//   - Los pases de sombra/velocidad/depth del interior (bcab3136e9259a72,
//     387becfeceeb4ca8): NO son exclusivos, comparten bytecode con `character`,
//     `character_props`, `fe_track` y `gps`. Usarlos meteria mallas que no son
//     del auto.
//   - Los car_basic_vertex (ca47451674befcfc, a43a55519308ca7c): el pack los usa
//     para car_wheel, car_matt, carpaint_* ... son la CARROCERIA EXTERIOR, que
//     tambien identifica al auto, pero se ve en todas las camaras y no distingue
//     el auto del jugador del de un rival en pantalla.
//   - Parabrisas y redes (vp_windscreen, vp_netting): exclusivos pero son piezas
//     chicas que nunca van a ser el maximo; solo agregarian ruido.
//
// RIESGO RESIDUAL, documentado y no resuelto: en rallycross puede haber otro
// auto en pantalla con su habitaculo visible. Su malla podria ganar el maximo de
// un frame. Igual que en DR2, se acepta: el consumidor de la huella exige que se
// mantenga varios frames seguidos antes de hacerle caso.
//
// CONTEXTOS DIFERIDOS (regla dura 5): esto corre en cualquier contexto y el
// "maximo del frame" es un global sin candado. Una carrera puede hacer que un
// frame suelto publique la malla equivocada. Es tolerable a proposito, por lo
// mismo de arriba. Un candado por draw costaria mas que el problema que evita.
//
// ---------------------------------------------------------------------------
// AVISO PARA EL AGENTE DEL INI (regla dura 11):
// esta funcion ESCRIBE iniParams[8].x (= x8) cada frame. No usar x8 para nada
// mas desde el ini. La fila 8 ya existe (el ini nombra y8), asi que el tamano de
// IniParams no cambia; igual se comprueba size() >= 9 antes de escribir.
//
// Y LA TRAMPA DEL FLOAT, que ya mordio en DR2: la huella son 24 bits, o sea que
// vale hasta 16.777.215 y esta MUY por encima de 2^23 = 8.388.608. Arriba de
// 2^23 el paso del float32 es 2, asi que una comparacion del tipo
// "abs(x8 - guardado) < 0.5" COLAPSA: el borde se redondea al propio valor y
// "igual" da falso. Si el ini tiene que comparar la huella, la comparacion va
// por DIFERENCIA (x8 - guardado == 0), nunca por ventana.
// ===== LA HUELLA DE MALLA SE FUE (2026-09-05) ==============================
// Aca vivian TRIPLE_CAR_VS (los vertex shaders del habitaculo), tt_es_vs_de_interior
// y TripleTrackCarMesh, que publicaban en x8 el hash del index buffer mas grande
// dibujado con esos shaders. Fallo dos veces en el juego (habitaculos compartidos
// entre autos, huella heredada) y desde que x8 lo publica tt_car_update() con el
// id de modelo del motor no le quedaba ni el log. Con ella se fue el hash de
// index buffer por draw de HackerContext::BeforeDraw. El historial de git la tiene.


// Llamada desde HackerContext cuando el juego escribe un constant buffer que
// parece la camara principal en perspectiva (cb3 = CameraParamsConstantBuffer).
// Se captura del lado CPU: nada de readback de GPU.

// ===== Detector de REPETICION: se le pregunta al motor (2026-09-05) =========
//
// El mod se apaga solo mientras DiRT esta en una repeticion (la de fondo del
// resultado y la que se mira con controles). NO adivina por FOV, near, far ni
// por que shaders corren: todas esas huellas se midieron y se caen
// (docs\INVESTIGACION-DETECTAR-REPLAY.md). Lee el estado que el propio motor
// consulta, igual que hizo DR2 (su detector v2).
//
// QUE SE LEE. drt.exe 1.1.114.5105 (PE timestamp 0x5C795702, 15.481.856 bytes;
// es la unica version que existe, el juego no se actualiza desde 2016):
//
//   [exe+0xDC6038]  = puntero al neon::NeGameResource activo. Lo escribe
//                     exe+0x2FA120 cuando se arma una corrida, copiando un
//                     struct de contexto (exe+0x2FA132: `mov [0x11C6038], edx`).
//   NeGameResource+4 = tabla de componentes, indexada por enteros globales que
//                     el juego registra por NOMBRE al arrancar:
//   [exe+0xDC5DD8]  = indice de "neRacing::ReplaySystem"  (stub exe+0xB054E0)
//   [exe+0xDC5F94]  = indice de "game::ReplayDirector"    (stub exe+0xB05CE0)
//   ReplaySystem+0x1A0 = ESTADO (enum 0..14). Lo escribe ReplaySystem::SetState
//                     (exe+0x6F5170, `cmp edi,[esi+0x1A0]` en el prologo) y lo
//                     comparan 41 funciones del juego. Medido en el desensamblado:
//                       7  = GRABANDO: lo pone la salida a pista, el reinicio,
//                            la vuelta desde la repeticion y el splitscreen.
//                            O sea: manejando.
//                       0  = inactivo (cutscenes, "event", sin corrida cargada)
//                       1  = reproduciendo (el boton de la pantalla de replay
//                            dice "lng_replay_pause" cuando vale 1)
//                       2  = pausado (el mismo boton lo alterna 1 <-> 2; la
//                            pantalla de resultados lo pone con
//                            "result_screen_is_on")
//                       3,4,5,6,0xA,0xC = variantes de reproduccion (avance,
//                            rebobinado, salto: SetState las trata como la
//                            misma familia que 1 y 2)
//                       0xB = repeticion instantanea por choque ("largecoll")
//                       0xD,0xE = fin/limite de la reproduccion
//                       8,9 = no aparecen en ningun `push imm`: desconocidos
//   ReplayDirector+0x3C = MODO (0..12). StateReplay::Enter pone 0xB
//                     (exe+0x131CE9) y Exit restaura el anterior; 1 = go_to_race.
//                     Se registra como sonda; no decide.
//
// QUE DECIDE. $en_replay = 1 solo si el estado del ReplaySystem esta en la
// familia de REPRODUCCION {1,2,3,4,5,6,10,11,12,13,14}. 7 (grabando) y 0
// (inactivo) son "manejando" o "no hay corrida": mod activo. 8 y 9 son
// desconocidos y tambien dejan el mod activo (fail-open). Un valor fuera de
// 0..14 es basura (puntero viejo, estructura distinta): fail-open.
//
// NO SE HOOKEA NI SE ESCRIBE NADA. Antes de creerle a una sola lectura se
// validan por bytes exactos (con mascara sobre las direcciones absolutas,
// porque el exe tiene ASLR) los siete sitios de arriba; si uno no coincide el
// detector queda inerte y el mod sigue prendido. Todas las lecturas van dentro
// de __try, y cualquier fallo vuelve a "activo". Nunca se apaga manejando por
// una firma aproximada: la regla que dejo escrita el autor el 2026-08-16 es que
// un apagado de mas en pista cuesta mas que un artefacto en una repeticion.
//
// COMO SE COMPRUEBA EN EL JUEGO: d3d11_log.txt deja `TTREPLAY: estado=REPLAY/
// JUEGO rs=N dir=M` en cada transicion y `TTFLOW: rs=N dir=M` cada vez que
// cambia cualquiera de los dos numeros. Con una pasada (pista -> resultado con
// replay de fondo -> repeticion con controles -> pista) se lee cual estado
// tuvo cada pantalla, sin adivinar.
static volatile LONG g_tt_replay_active = 0;
static volatile LONG g_tt_replay_probe_state = 0; // 0=nuevo, 2=inicializando, 1=listo, -1=no compatible
static volatile LONG g_tt_replay_last = -1;
static volatile LONG g_tt_replay_read_fault = 0;
static unsigned g_tt_replay_frame = 0xFFFFFFFF;
static unsigned char *g_tt_exe = NULL;
static CommandListVariable *g_tt_replay_ini = NULL;
static int g_tt_probe_rs = -9999, g_tt_probe_dir = -9999;

#define TT_RVA_CTX_GAMERES    0xDC6038u
#define TT_RVA_IDX_REPLAYSYS  0xDC5DD8u
#define TT_RVA_IDX_DIRECTOR   0xDC5F94u
#define TT_OFF_RS_ESTADO      0x1A0
#define TT_OFF_DIR_MODO       0x3C

static bool tt_replay_activo()
{
	return InterlockedCompareExchange(&g_tt_replay_active, 0, 0) != 0;
}

// Compara `n` bytes en exe+rva contra `pat`, salteando los que `mask` marque
// con 0 (los operandos absolutos, que ASLR reubica).
static bool tt_firma(const unsigned char *exe, unsigned rva, const unsigned char *pat, const unsigned char *mask, size_t n)
{
	const unsigned char *p = exe + rva;
	for (size_t i = 0; i < n; i++)
		if (mask[i] && p[i] != pat[i])
			return false;
	return true;
}

static bool tt_dword_es(const unsigned char *exe, unsigned rva, const void *esperado)
{
	return *(const DWORD *)(exe + rva) == (DWORD)(uintptr_t)esperado;
}

static bool tt_string_en(const unsigned char *exe, unsigned rva_del_operando, const char *texto)
{
	const char *s = (const char *)(uintptr_t)*(const DWORD *)(exe + rva_del_operando);
	return s && strncmp(s, texto, strlen(texto)) == 0;
}

static bool tt_replay_init()
{
	LONG estado = InterlockedCompareExchange(&g_tt_replay_probe_state, 2, 0);
	if (estado == 1)
		return true;
	if (estado == -1 || estado == 2)
		return false;

	unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
	if (!exe) {
		LogInfo("TTREPLAY: no pude obtener la base del exe; detector inerte.\n");
		InterlockedExchange(&g_tt_replay_probe_state, -1);
		return false;
	}

	// 1. stub que registra el indice de neRacing::ReplaySystem:
	//    push <str>; call rel; add esp,4; mov [idx], eax
	static const unsigned char s_idx_rs[]  = { 0x68,0,0,0,0, 0xE8,0,0,0,0, 0x83,0xC4,0x04, 0xA3,0,0,0,0 };
	static const unsigned char m_idx[]     = { 1,0,0,0,0, 1,0,0,0,0, 1,1,1, 1,0,0,0,0 };
	// 2. idem game::ReplayDirector (misma forma)
	// 3. ReplaySystem::SetState: push ebp; mov ebp,esp; sub esp,10; push esi; push edi;
	//    mov edi,[ebp+8]; mov esi,ecx; cmp edi,[esi+0x1A0]
	static const unsigned char s_setstate[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x10,0x56,0x57,0x8B,0x7D,0x08,0x8B,0xF1,0x3B,0xBE,0xA0,0x01,0x00,0x00 };
	// 4. ReplayDirector::SetMode: ... cmp dword [esi+0x3C],5 ...
	static const unsigned char s_setmode[]  = { 0x55,0x8B,0xEC,0x56,0x8B,0xF1,0x83,0x7E,0x3C,0x05,0x57,0x8B,0x7D,0x08,0x75,0x15,0x83,0xFF,0x05,0x74,0x10 };
	// 5. un consumidor de la tabla: mov ecx,[ctx]; mov edx,[ecx+4]; mov eax,[idx]; mov ecx,[edx+eax*4]
	static const unsigned char s_tabla[]    = { 0x8B,0x0D,0,0,0,0, 0x8B,0x51,0x04, 0xA1,0,0,0,0, 0x8B,0x0C,0x82 };
	static const unsigned char m_tabla[]    = { 1,1,0,0,0,0, 1,1,1, 1,0,0,0,0, 1,1,1 };
	// 6. quien escribe el puntero del contexto: ... mov [0x11C6034],ecx; mov edx,[eax+4]; mov [0x11C6038],edx
	static const unsigned char s_setctx[]   = { 0x55,0x8B,0xEC,0x53,0x8B,0x45,0x08,0x8B,0x08,0x89,0x0D,0,0,0,0, 0x8B,0x50,0x04,0x89,0x15,0,0,0,0 };
	static const unsigned char m_setctx[]   = { 1,1,1,1,1,1,1,1,1,1,1,0,0,0,0, 1,1,1,1,1,0,0,0,0 };
	// 7. cola de StateReplay::Enter: mov eax,[esi+0xC]; mov byte [esi+0x10],1; ...; push "flow_replay_enter"; call edx
	static const unsigned char s_enter[]    = { 0x8B,0x46,0x0C, 0xC6,0x46,0x10,0x01, 0x8B,0x48,0x04, 0x8B,0x15,0,0,0,0, 0x8B,0x0C,0x91, 0x8B,0x01, 0x8B,0x50,0x04, 0x68,0,0,0,0, 0xFF,0xD2 };
	static const unsigned char m_enter[]    = { 1,1,1, 1,1,1,1, 1,1,1, 1,1,0,0,0,0, 1,1,1, 1,1, 1,1,1, 1,0,0,0,0, 1,1 };
	static const unsigned char m_todo[32]   = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };

	bool ok = false;
	__try {
		ok = tt_firma(exe, 0xB054E0, s_idx_rs, m_idx, sizeof(s_idx_rs))
		  && tt_dword_es(exe, 0xB054E0 + 14, exe + TT_RVA_IDX_REPLAYSYS)
		  && tt_string_en(exe, 0xB054E0 + 1, "neRacing::ReplaySystem")
		  && tt_firma(exe, 0xB05CE0, s_idx_rs, m_idx, sizeof(s_idx_rs))
		  && tt_dword_es(exe, 0xB05CE0 + 14, exe + TT_RVA_IDX_DIRECTOR)
		  && tt_string_en(exe, 0xB05CE0 + 1, "game::ReplayDirector")
		  && tt_firma(exe, 0x6F5170, s_setstate, m_todo, sizeof(s_setstate))
		  && tt_firma(exe, 0x23FDC0, s_setmode, m_todo, sizeof(s_setmode))
		  && tt_firma(exe, 0x0359F6, s_tabla, m_tabla, sizeof(s_tabla))
		  && tt_dword_es(exe, 0x0359F6 + 2, exe + TT_RVA_CTX_GAMERES)
		  && tt_firma(exe, 0x2FA120, s_setctx, m_setctx, sizeof(s_setctx))
		  && tt_dword_es(exe, 0x2FA120 + 20, exe + TT_RVA_CTX_GAMERES)
		  && tt_firma(exe, 0x131D09, s_enter, m_enter, sizeof(s_enter))
		  && tt_string_en(exe, 0x131D09 + 25, "flow_replay_enter");
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTREPLAY: las firmas de drt.exe 1.1.114.5105 no coinciden; "
			"detector inerte y mod activo (fail-open).\n");
		LogOverlay(LOG_WARNING, "TrueTriple: detector de repeticion no compatible; el mod queda activo\n");
		InterlockedExchange(&g_tt_replay_probe_state, -1);
		return false;
	}
	g_tt_replay_ini = cl_global(L"$en_replay");
	if (!g_tt_replay_ini) {
		LogInfo("TTREPLAY: falta global $en_replay en el ini; detector inerte.\n");
		InterlockedExchange(&g_tt_replay_probe_state, -1);
		return false;
	}
	g_tt_exe = exe;
	LogInfo("TTREPLAY: detector listo (7 firmas OK); ReplaySystem+0x1A0 decide, "
		"ReplayDirector+0x3C se registra (solo lectura).\n");
	InterlockedExchange(&g_tt_replay_probe_state, 1);
	return true;
}

static void tt_replay_update()
{
	if (InterlockedCompareExchange(&g_tt_replay_probe_state, 0, 0) != 1 &&
		!tt_replay_init())
		return;
	// Una vez por frame alcanza: el estado cambia por pantallas, no por draws.
	if (g_tt_replay_frame == G->frame_no)
		return;
	g_tt_replay_frame = G->frame_no;

	int rs = -1, dir = -1;
	bool lectura_ok = true;
	__try {
		unsigned char *exe = g_tt_exe;
		unsigned char *ctx = *(unsigned char **)(exe + TT_RVA_CTX_GAMERES);
		if ((uintptr_t)ctx < 0x10000) {
			// Sin corrida cargada (o antes de la primera): no hay repeticion.
			rs = 0; dir = 0;
		} else {
			unsigned char **tabla = *(unsigned char ***)(ctx + 4);
			int i_rs  = *(int *)(exe + TT_RVA_IDX_REPLAYSYS);
			int i_dir = *(int *)(exe + TT_RVA_IDX_DIRECTOR);
			if ((uintptr_t)tabla < 0x10000 || i_rs < 0 || i_rs > 8192 || i_dir < 0 || i_dir > 8192)
				lectura_ok = false;
			else {
				unsigned char *p_rs  = tabla[i_rs];
				unsigned char *p_dir = tabla[i_dir];
				if ((uintptr_t)p_rs < 0x10000)
					lectura_ok = false;
				else {
					rs = *(int *)(p_rs + TT_OFF_RS_ESTADO);
					dir = ((uintptr_t)p_dir >= 0x10000) ? *(int *)(p_dir + TT_OFF_DIR_MODO) : -1;
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		lectura_ok = false;
	}
	// Fuera del enum conocido = basura (puntero viejo o estructura distinta).
	if (lectura_ok && (rs < 0 || rs > 14))
		lectura_ok = false;

	if (!lectura_ok) {
		// Nunca conservar un 1 viejo si la estructura dejo de ser legible: eso
		// apagaria el mod manejando. El fallo seguro es volver a activo.
		InterlockedExchange(&g_tt_replay_active, 0);
		g_tt_replay_ini->fval = 0.0f;
		if (InterlockedCompareExchange(&g_tt_replay_read_fault, 1, 0) == 0)
			LogInfo("TTREPLAY: fallo leyendo el estado; mod activo (fail-open).\n");
		return;
	}

	if (rs != g_tt_probe_rs || dir != g_tt_probe_dir) {
		LogInfo("TTFLOW: rs=%d dir=%d\n", rs, dir);
		g_tt_probe_rs = rs; g_tt_probe_dir = dir;
	}

	// Familia de REPRODUCCION del ReplaySystem. 0 (inactivo) y 7 (grabando)
	// son "manejando"; 8 y 9 no aparecen en el exe y se tratan como manejando.
	static const unsigned char reproduce[15] = { 0,1,1,1,1,1,1,0,0,0,1,1,1,1,1 };
	LONG activo = reproduce[rs] ? 1 : 0;
	InterlockedExchange(&g_tt_replay_active, activo);
	g_tt_replay_ini->fval = activo ? 1.0f : 0.0f;

	LONG anterior = InterlockedExchange(&g_tt_replay_last, activo);
	if (anterior != activo) {
		LogInfo("TTREPLAY: estado=%s rs=%d dir=%d\n", activo ? "REPLAY" : "JUEGO", rs, dir);
		if (activo)
			LogOverlay(LOG_WARNING, "TrueTriple: pausado durante la repeticion\n");
		else if (anterior >= 0)
			LogOverlay(LOG_WARNING, "TrueTriple: activo\n");
	}
}

// ===== Detector de AUTO: el id de modelo que tiene el motor (2026-09-05) ====
//
// El plantel (TrueTriple-DR1.autos.txt) numera los presets de asiento por auto.
// Hasta aca el auto lo declaraba el usuario con Ctrl+RePag/AvPag, porque la
// unica deteccion automatica que hubo (hash de la malla mas grande del
// habitaculo) fallo en el juego: dos autos comparten habitaculo y la huella se
// hereda. Pedido del autor: reconocer el auto, en lo posible en cualquier camara.
//
// QUE SE LEE. El propio motor sabe que auto cargo. La lista de vehiculos
// cargados vive en el "Car Manager" del neRacing::AudioManager (componente de
// la MISMA tabla del NeGameResource que el ReplaySystem; indice en
// [exe+0xDC5EAC], stub exe+0xB05320; el Car Manager es el sub-objeto en
// AudioManager+0x2280, registrado como "Audio:Updateables:Car Manager"), en
// CarManager+0xB114: un PUNTERO a la cabecera de una lista enlazada
// (exe+0x6B5D2F: `mov eax,[ecx+0xB114]; mov esi,[eax]`, corta al volver a eax).
// exe+0x6B5D20 la recorre para cargar "cars/models/%s/bundles/" con
// [entrada+0x6C] = puntero al id de modelo (las 3 letras de cars\models\<id>:
// i20, aqb, 037...), y exe+0x6C4C70 separa las entradas con [entrada+0x68] != 0
// (las de la IA: van al vector de IA y cargan los bundles de "ai/") de las del
// jugador ([+0x68] == 0). O sea: la primera entrada con +0x68 == 0 es el auto
// del jugador, y su +0x6C dice cual es. Que sea el manager de AUDIO no cambia
// nada: son los mismos vehiculos, con el mismo id, y la lista vive mientras la
// corrida esta cargada.
//
// PRIMERA CORRIDA (2026-09-05): se leyo el neRacing::VehicleSystem (indice
// [0xDC5F40]) creyendo que era el dueño de la lista, y no lo es (su vector de
// vehiculos esta en +0xC4 y no guarda el id como texto): vehiculos=0 siempre.
// exe+0x418450 es la prueba del camino correcto: carga [0xDC5EAC], indexa la
// tabla y entra derecho a +0x2280.
//
// No depende de la camara ni de la malla: vale en cockpit, capo, persecucion,
// repeticion y garage de pre-carrera, mientras el vehiculo este cargado. En
// menus sin corrida no hay lista y x8 queda en 0 (la identidad declarada sigue
// mandando: fail-open, igual que hasta hoy).
//
// LA LISTA ES TRANSITORIA (medido el 2026-09-05, tercera corrida): se llena
// durante la carga de la corrida y se vacia en cuanto el cargador crea los
// objetos de audio de cada vehiculo. El log lo mostro asi:
//     TTCAR: jugador=i20 -> N=15 | vehiculos=1: i20/jugador
//     (cinco lineas despues)  jugador=(ninguno) -> N=0 | vehiculos=0
// y el ini pide 12 frames seguidos para creerle a x8, asi que nunca latcheaba.
// Por eso el numero se RETIENE: el ultimo id visto sigue publicado hasta que
// aparezca otro (la proxima corrida vuelve a llenar la lista con el auto nuevo
// antes de largar). En los menus queda el ultimo auto, que es lo mismo que
// hacia la identidad declarada. Si se quiere volver al declarado a mano, sigue
// estando Ctrl+RePag/AvPag: el retenido solo pisa mientras haya un id.
//
// QUE PUBLICA. x8 = NUMERO DEL PLANTEL (1..N) del auto cuyo id coincide con la
// tercera columna del .txt, o 0 si nunca se vio un auto o el id no esta en la
// lista. El
// ini ya tenia la estabilidad de 12 frames y el cruce por $car_hash_N; con los
// pares horneados como identidad (k -> k) el numero pasa derecho a $car_match.
// Log: `TTCAR: jugador=<id> -> N (<nombre>) | vehiculos: id/ai ...` en cada
// cambio, para que la primera corrida diga si la tabla id->nombre esta bien.
//
// Mismas reglas que el detector de replay: firmas de bytes con mascara (ASLR),
// solo lectura dentro de __try, y ante cualquier duda x8 = 0.
int tt_autos_por_id(const char *id);   // Overlay.cpp (lee el .txt del plantel)

static volatile LONG g_tt_car_probe_state = 0;
static unsigned g_tt_car_frame = 0xFFFFFFFF;
static char g_tt_car_id_ultimo[16] = "";
static int g_tt_car_n_ultimo = -1;
static char g_tt_car_id_ret[16] = "";   // el ultimo id visto (retenido)
static int g_tt_car_n_ret = 0;          // y su numero de plantel

#define TT_RVA_IDX_AUDIOMGR 0xDC5EACu
#define TT_OFF_AM_CARMGR    0x2280
#define TT_OFF_CM_LISTA     0xB114
#define TT_OFF_VEH_AI       0x68
#define TT_OFF_VEH_ID       0x6C

static bool tt_car_init()
{
	LONG estado = InterlockedCompareExchange(&g_tt_car_probe_state, 2, 0);
	if (estado == 1)
		return true;
	if (estado == -1 || estado == 2)
		return false;
	unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
	if (!exe) {
		InterlockedExchange(&g_tt_car_probe_state, -1);
		return false;
	}
	// stub del indice: push <str>; call rel; add esp,4; mov [idx], eax
	static const unsigned char s_idx[]    = { 0x68,0,0,0,0, 0xE8,0,0,0,0, 0x83,0xC4,0x04, 0xA3,0,0,0,0 };
	static const unsigned char m_idx[]    = { 1,0,0,0,0, 1,0,0,0,0, 1,1,1, 1,0,0,0,0 };
	// cargador de bundles: push ebp; mov ebp,esp; sub esp,0x118; mov eax,[ecx+0x1C];
	// mov edx,[eax+4]; mov eax,[ecx+0xB114]
	static const unsigned char s_carga[]  = { 0x55,0x8B,0xEC,0x81,0xEC,0x18,0x01,0x00,0x00,0x8B,0x41,0x1C,0x8B,0x50,0x04,0x8B,0x81,0x14,0xB1,0x00,0x00 };
	// uso del id: mov edx,[esi+0x6C]; push edx; lea eax,[ebp-0x118]; push "cars/models/%s/bundles/"; push eax
	static const unsigned char s_id[]     = { 0x8B,0x56,0x6C, 0x52, 0x8D,0x85,0xE8,0xFE,0xFF,0xFF, 0x68,0,0,0,0, 0x50 };
	static const unsigned char m_id[]     = { 1,1,1, 1, 1,1,1,1,1,1, 1,0,0,0,0, 1 };
	// la bandera de IA: cmp byte [edi+0x68],0; mov eax,[edi+0x5C]
	static const unsigned char s_ai[]     = { 0x80,0x7F,0x68,0x00, 0x8B,0x47,0x5C };
	// el camino al Car Manager (exe+0x418450): mov eax,[ecx+0x114]; mov ecx,[eax+4];
	// mov edx,[idx_audiomgr]; mov ecx,[ecx+edx*4]; push 0; lea ecx,[ecx+0x2280]
	static const unsigned char s_cm[]     = { 0x8B,0x81,0x14,0x01,0x00,0x00, 0x8B,0x48,0x04, 0x8B,0x15,0,0,0,0, 0x8B,0x0C,0x91, 0x6A,0x00, 0x8D,0x89,0x80,0x22,0x00,0x00 };
	static const unsigned char m_cm[]     = { 1,1,1,1,1,1, 1,1,1, 1,1,0,0,0,0, 1,1,1, 1,1, 1,1,1,1,1,1 };
	static const unsigned char m_todo[24] = { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };
	bool ok = false;
	__try {
		ok = tt_firma(exe, 0xB05320, s_idx, m_idx, sizeof(s_idx))
		  && tt_dword_es(exe, 0xB05320 + 14, exe + TT_RVA_IDX_AUDIOMGR)
		  && tt_string_en(exe, 0xB05320 + 1, "neRacing::AudioManager")
		  && tt_firma(exe, 0x018450, s_cm, m_cm, sizeof(s_cm))
		  && tt_dword_es(exe, 0x018450 + 11, exe + TT_RVA_IDX_AUDIOMGR)
		  && tt_firma(exe, 0x6B5D20, s_carga, m_todo, sizeof(s_carga))
		  && tt_firma(exe, 0x6B5D7F, s_id, m_id, sizeof(s_id))
		  && tt_string_en(exe, 0x6B5D7F + 11, "cars/models/%s/bundles/")
		  && tt_firma(exe, 0x6C4C70, s_ai, m_todo, sizeof(s_ai));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTCAR: las firmas de drt.exe 1.1.114.5105 no coinciden; el auto queda "
			"como lo declare el usuario (fail-open).\n");
		InterlockedExchange(&g_tt_car_probe_state, -1);
		return false;
	}
	g_tt_exe = exe;
	LogInfo("TTCAR: detector de auto listo (AudioManager+0x2280 -> CarManager+0xB114, id en +0x6C, IA en +0x68).\n");
	InterlockedExchange(&g_tt_car_probe_state, 1);
	return true;
}

static bool tt_id_valido(const char *id)
{
	size_t n = strlen(id);
	if (n < 2 || n > 6)
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

static void tt_car_update()
{
	if (InterlockedCompareExchange(&g_tt_car_probe_state, 0, 0) != 1 && !tt_car_init())
		return;
	if (g_tt_car_frame == G->frame_no)
		return;
	g_tt_car_frame = G->frame_no;

	char jugador[16] = "";
	char lista[160] = "";
	int nveh = 0;
	__try {
		unsigned char *exe = g_tt_exe;
		unsigned char *ctx = *(unsigned char **)(exe + TT_RVA_CTX_GAMERES);
		if ((uintptr_t)ctx >= 0x10000) {
			unsigned char **tabla = *(unsigned char ***)(ctx + 4);
			int idx = *(int *)(exe + TT_RVA_IDX_AUDIOMGR);
			if ((uintptr_t)tabla >= 0x10000 && idx >= 0 && idx <= 8192) {
				unsigned char *am = tabla[idx];
				unsigned char *vs = ((uintptr_t)am >= 0x10000) ? am + TT_OFF_AM_CARMGR : NULL;
				if ((uintptr_t)vs >= 0x10000) {
					// +0xB114 guarda un PUNTERO a la cabecera de la lista, no la
					// cabecera (exe+0x6B5D2F: `mov eax,[ecx+0xB114]; mov esi,[eax]`,
					// y el bucle corta cuando esi vuelve a eax). La primera corrida
					// (2026-09-05) lo leyo como cabecera en linea y vio 0 vehiculos.
					unsigned char *cab = *(unsigned char **)(vs + TT_OFF_CM_LISTA);
					unsigned char *nodo = ((uintptr_t)cab >= 0x10000) ? *(unsigned char **)cab : cab;
					for (int k = 0; k < 64 && (uintptr_t)cab >= 0x10000 && nodo != cab && (uintptr_t)nodo >= 0x10000; k++) {
						unsigned char ai = nodo[TT_OFF_VEH_AI];
						const char *idp = *(const char **)(nodo + TT_OFF_VEH_ID);
						char id[16] = "";
						if ((uintptr_t)idp >= 0x10000) {
							for (int i = 0; i < 15 && idp[i]; i++)
								id[i] = (idp[i] >= 'A' && idp[i] <= 'Z') ? idp[i] - 'A' + 'a' : idp[i];
						}
						if (!tt_id_valido(id)) {
							// Estructura que no es la esperada: no seguir leyendo, y
							// decir UNA vez que se vio, para no perseguirlo a ciegas.
							static bool avisado = false;
							if (!avisado) {
								avisado = true;
								LogInfo("TTCAR: nodo %d sin id valido (ai=%u, id='%s'): corto la lista.\n", k, ai, id);
							}
							break;
						}
						nveh++;
						if (!ai && !jugador[0])
							strcpy_s(jugador, sizeof(jugador), id);
						size_t l = strlen(lista);
						if (l + 12 < sizeof(lista))
							_snprintf_s(lista + l, sizeof(lista) - l, _TRUNCATE, " %s/%s", id, ai ? "ia" : "jugador");
						nodo = *(unsigned char **)nodo;
					}
				}
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		jugador[0] = 0; nveh = 0;
	}

	// Se retiene el ultimo id visto: la lista se vacia en cuanto termina la
	// carga (ver arriba). Un id nuevo reemplaza al anterior.
	if (jugador[0] && strcmp(jugador, g_tt_car_id_ret) != 0) {
		strcpy_s(g_tt_car_id_ret, sizeof(g_tt_car_id_ret), jugador);
		g_tt_car_n_ret = tt_autos_por_id(jugador);
		if (g_tt_car_n_ret == 0)
			LogInfo("TTCAR: el id '%s' NO esta en TrueTriple-DR1.autos.txt: se usa el auto declarado.\n", jugador);
	}
	int n = g_tt_car_n_ret;
	if (G->iniParams.size() >= 9)
		G->iniParams[8].x = (float)n;

	if (n != g_tt_car_n_ultimo || strcmp(jugador, g_tt_car_id_ultimo) != 0) {
		LogInfo("TTCAR: jugador=%s -> N=%d (retenido=%s) | vehiculos=%d:%s\n",
			jugador[0] ? jugador : "(ninguno)", n, g_tt_car_id_ret[0] ? g_tt_car_id_ret : "-", nveh, lista);
		g_tt_car_n_ultimo = n;
		strcpy_s(g_tt_car_id_ultimo, sizeof(g_tt_car_id_ultimo), jugador);
	}
}

// ===== Camara del motor: hook INERTE del constructor de proyeccion (2026-09-04) =====
//
// Rama `culling`. Primer paso del plan A de docs\INVESTIGACION-CULLING.md: el
// motor descarta en la CPU todo lo que cae fuera de SU frustum (~150 grados
// horizontales con el FOV al maximo), y el puesto de tres monitores abarca ~182.
// La idea es imponerle a la camara del jugador un FOV horizontal de ~172 grados
// ANTES de que construya su proyeccion, para que todo lo que decide la CPU
// (descarte, LOD, streaming, siembra del pasto) lo vea ancho. Al mod no le cambia
// nada: de cb3 solo toma la base de camara normalizada y la fila de profundidad.
//
// LA PRIMERA CORRIDA (inerte, 2026-09-04) dijo: el hook no rompe nada; hay
// muchas camaras (ortograficas de aspecto 5.3889 = sombras/UI, perspectivas de
// aspecto 1.0 = envmap, dos de fov 90 aspecto 5.3889 con 15 y 5 llamadas por
// frame), y el juego crea camaras nuevas todo el tiempo (la tabla por puntero de
// objeto se desbordo con ~46 llamadas/frame sin anotar). Por eso ahora se anota
// por SITIO DE LLAMADA + forma (persp, fov, aspecto), no por objeto.
//
// EL ENSANCHE ($cull_widen, tecla B). Para toda camara en PERSPECTIVA con aspecto
// de triple (3.6..12) y fov < 100, se reemplaza el ASPECTO (+0xF0) por el que hace
// que el horizontal de 2*atan(aspecto*tan(fov/2)) valga TT_CULL_HFOV grados, y
// recien despues se llama a la original. Por aspecto y no por fov, a proposito:
//   - el vertical no cambia, asi que ni el LOD por tamano en pantalla ni las
//     sombras (si se ajustan al frustum vertical) se mueven;
//   - m11 no cambia, asi que la lista blanca de m11 del ini sigue valiendo;
//   - es idempotente: el objetivo sale del fov, no del aspecto anterior.
// Con fov >= 100 el objetivo caeria por debajo de 12 y se confundiria con un
// aspecto original: no se toca. El campo queda escrito (no se restaura tras la
// llamada) para que lo vea todo lector posterior; si el usuario apaga la tecla,
// la proxima construccion con aspecto >= 12 lo vuelve al ultimo original visto.
// Se cuentan las APLICACIONES: un ensanche que no corre no es un ensanche (leccion
// de FH6). Lo que se anota:
//   - que camaras pasan por aca, con su aspecto, FOV, near y far, veces por frame;
//   - desde donde las llaman (direccion de retorno, ya con el ASLR descontado);
//   - que matriz sale (m00, m11, m22, m23), para cruzarla con las lineas
//     `TRIPLE camera #N` que ya deja TripleCaptureCameraCB y saber cual de estas
//     camaras es la que llega a cb3.
//
// QUE SE ENGANCHA. drt.exe 1.1.114.5105 tiene UNA clase de camara (vtable en
// exe+0xCE0860, constructor exe+0x52B930). Campos medidos en el desensamblado:
//   +0xE0 byte  1 = perspectiva / 0 = ortografica
//   +0xE4 float FOV VERTICAL en GRADOS (el ctor pone 45.0)
//   +0xE8 float near (0.1)      +0xEC float far (10000)
//   +0xF0 float aspecto (1.0)   +0x110 la matriz de PROYECCION (4x4, 64 bytes)
// El slot 16 de la vtable (exe+0xCE08A0) es "construir la proyeccion"
// (exe+0x4EC6F0): tan(pi/2 - fov/2) da el cot, m00 = cot/aspecto, m11 = cot,
// m22 = (n+f)/(n-f), m23 = 2nf/(n-f); lo copia a +0x110. Su unico llamador
// directo es el constructor: las reconstrucciones por frame van por la vtable.
//
// COMO. Se reemplaza el puntero del slot 16 (.rdata, un solo dword) por
// tt_cam_build_hook, que anota y llama a la original. No se parchea codigo. Tres
// firmas antes de tocar: los 61 bytes del prologo de la original, el tramo del
// constructor que escribe +0xE4..+0x100 y pone la vtable, y los slots 16, 17 y
// 18 de la vtable apuntando a donde tienen que apuntar. Fail-open: si algo no
// coincide, no se instala y el mod sigue igual. El puntero original se guarda y
// se restaura cuando el dll se descarga (destructor estatico, que corre en
// DLL_PROCESS_DETACH con el dll todavia mapeado).
//
// Convencion: la original es __thiscall sin argumentos (termina en `ret` pelado).
// Un __fastcall(self, edx) tiene exactamente ese ABI: `this` en ecx, nada en la
// pila, y el callee no limpia nada.

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#define TT_RVA_CAM_VTABLE   0xCE0860u
#define TT_RVA_CAM_BUILD    0x4EC6F0u
#define TT_RVA_CAM_SUBRECT  0x4EC970u
#define TT_RVA_CAM_SLOT18   0x4EBFA0u
#define TT_RVA_CAM_CTOR_E4  0x52B97Du
#define TT_CAM_SLOT_BUILD   16
#define TT_OFF_CAM_PERSP    0xE0
#define TT_OFF_CAM_FOV      0xE4
#define TT_OFF_CAM_NEAR     0xE8
#define TT_OFF_CAM_FAR      0xEC
#define TT_OFF_CAM_ASPECT   0xF0
#define TT_OFF_CAM_PROJ     0x110

typedef void (__fastcall *tt_cam_build_fn)(void *self, void *edx_sin_uso);

static volatile LONG g_tt_cam_state = 0;        // 0 sin probar, 2 probando, 1 instalado, -1 inerte
static tt_cam_build_fn g_tt_cam_build_orig = NULL;
static DWORD *g_tt_cam_slot = NULL;             // &vtable[16] en el exe
static unsigned char *g_tt_cam_exe = NULL;
static CRITICAL_SECTION g_tt_cam_cs;

// Horizontal objetivo del frustum del motor, en grados. 172: por debajo de los
// 180 que una matriz no puede representar, y por encima de lo que cubre el
// puesto salvo 2-3 grados pegados al costado del ojo.
// Build 6 (2026-09-04): 172 -> 178. La corrida 5 mostro arbustos y terreno
// recuperados sin costo de fps, y quedaban trozos de terreno afuera. m00 pasa a
// 1/tan(89) = 0.01746: todavia lejos del limite del float, y a 1 grado del techo
// geometrico de una proyeccion plana.
// Build 7 (179.9, m00 = 0.00087): parpadeos con todo el terreno desapareciendo y
// el auto transparente. Con m00 tan chico la matriz deja de ser util para los
// efectos de pantalla y para lo que divide por ella. Build 8: se vuelve a 172,
// que fue la que el autor vio andar (arbustos y terreno recuperados sin costo).
// Lo que sigue faltando del terreno se busca en SU sistema, no subiendo esto.
#define TT_CULL_HFOV        172.0f

// Los 26 vertex shaders de terreno del shaderpack (ids `terrain_*`, sin ground
// cover), ordenados para busqueda binaria. HackerContext::VSSetShader llama a
// TripleNotaVS con cada hash bindeado; si es de terreno, se anota el frame. Es la
// senal de "hay un tramo cargado y dibujandose" que gatea el ensanche.
static const UINT64 g_tt_vs_terreno[] = {
	0x022444dc9a5cafe0ull, 0x02bf7a6f6da753ccull, 0x1d88c2d39858bba5ull, 0x203245d5ea2a196full,
	0x248f966608628a11ull, 0x37b90a5be592259cull, 0x3bfb5714596c71bfull, 0x3f0b7ce05cd77874ull,
	0x52d681b49309646eull, 0x569c9af4706c7fc5ull, 0x5fa6f29f28a2b57bull, 0x658e516ab8cde0ebull,
	0x83101ded40c463b1ull, 0x99ff2d8b13642546ull, 0x9eea420a7a4a5f21ull, 0xa0d13d244338d7e1ull,
	0xad8ebd74a5f11f4bull, 0xb79ffb6746d07d1dull, 0xba02505b655a4c80ull, 0xba3a83d8d505d66cull,
	0xbad211b901fbedf8ull, 0xbf4b46924b9bfa45ull, 0xd5b71821ece24f93ull, 0xe607ccbeb7f657e4ull,
	0xf024befebf9fce2cull, 0xf6b5073fd4c5ec39ull };
static volatile LONG g_tt_terreno_frame = 0;

// Hasta el 2026-11-10 aca vivian la lista de los 3 vertex shaders del
// PARABRISAS (`g_tt_vs_vidrio`, para que la niebla copiara el depth antes del
// vidrio) y el diagnostico `tt_fx_diag` (la linea `TTFX:` del log cada 300
// frames). Eran SOLO de la niebla por distancia, que se retiro de la plantilla
// en la v33; se sacaron del dll junto con las senales w8 (vidrio) y z8 (tramo)
// que publicaban. La lista de terreno de arriba se queda: gatea el ensanche.

void TripleNotaVS(UINT64 hash)
{
	int lo = 0, hi = (int)(sizeof(g_tt_vs_terreno) / sizeof(g_tt_vs_terreno[0])) - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (g_tt_vs_terreno[m] == hash) {
			InterlockedExchange(&g_tt_terreno_frame, (LONG)(G ? G->frame_no : 1));
			break;
		}
		if (g_tt_vs_terreno[m] < hash) lo = m + 1; else hi = m - 1;
	}
}
// ===== EL ENSANCHE NO PUEDE LLEGAR A LOS SHADERS (2026-09-04, build 19) =======
//
// EL BUG, y es de fondo, no un caso suelto: ensanchar la matriz de la escena
// abre el descarte de la CPU (que es lo que se busca) pero ESA MISMA matriz
// viaja al cbuffer de camara y la ven TODOS los shaders. Los que el mod parchea
// se recalibran solos desde las normas de fila de cb3 y salen bien; los que NO
// parchea -el polvo, el humo, las particulas `pfx_*`, el cielo y los 65
// escritores de SV_Position que no caen en ningun grupo- la usan tal cual y
// salen APLASTADOS a lo ancho. El autor lo reporto asi: primero los menus, y
// despues "hay efectos como el polvo que tambien estan aplastados".
//
// Tapar caso por caso (gatear por menu, por terreno, por lo que sea) no cierra
// nunca: la lista de shaders sin parchear no la controlamos.
//
// EL ARREGLO: el ensanche se queda del lado de la CPU. El motor arma la matriz
// ancha, hace su descarte con ella, y JUSTO ANTES de que el cbuffer llegue a la
// GPU se le devuelve el m00 original. `HackerContext::Unmap` ya tiene el puntero
// del mapeo y ya llama aca, asi que es un solo lugar y no cuesta un draw.
//
// Que se toca del cbuffer (layout verificado, ver el mapa de floats mas abajo):
//   f[0]                       projection m00
//   f[16]                      inverseProj [0][0] = 1/m00
//   f[36] f[40] f[44] f[48]    la componente .x de relativeViewProjection
// y nada mas: m00 no aparece en ningun otro lado de una perspectiva estandar.
//
// El m00 original NO se adivina: lo registran los dos hooks del ensanche
// (`tt_eye_hook` y el stub inline) indexado por m11, que el ensanche NO toca.
// Asi la restauracion es exacta aunque haya varias camaras por frame, y si el
// par no esta, no se toca nada.
struct tt_m00_par { float m11, m00; };
#define TT_M00_MAX 24
static tt_m00_par g_tt_m00[TT_M00_MAX];
static volatile LONG g_tt_m00_n = 0;
static volatile LONG g_tt_m00_restauradas = 0, g_tt_m00_sin_par = 0;

static void tt_m00_registrar(float m11, float m00_orig)
{
	if (!(m11 > 1e-6f) || !(m00_orig > 1e-9f))
		return;
	LONG n = InterlockedCompareExchange(&g_tt_m00_n, 0, 0);
	if (n > TT_M00_MAX) n = TT_M00_MAX;
	for (LONG i = 0; i < n; i++) {
		if (fabsf(g_tt_m00[i].m11 - m11) <= 1e-6f * m11) {
			g_tt_m00[i].m00 = m00_orig;
			return;
		}
	}
	if (n < TT_M00_MAX) {
		g_tt_m00[n].m11 = m11;
		g_tt_m00[n].m00 = m00_orig;
		InterlockedExchange(&g_tt_m00_n, n + 1);
	}
}

static float tt_m00_buscar(float m11)
{
	LONG n = InterlockedCompareExchange(&g_tt_m00_n, 0, 0);
	if (n > TT_M00_MAX) n = TT_M00_MAX;
	for (LONG i = 0; i < n; i++)
		if (fabsf(g_tt_m00[i].m11 - m11) <= 1e-6f * m11)
			return g_tt_m00[i].m00;
	return 0.0f;
}

// El m00 que deja el ensanche. Se compara contra el del cbuffer para saber si
// esa camara es una de las nuestras: es un valor fijo y reconocible.
static float g_tt_inl_m00_ancho = 0.0f;

// Deshace el ensanche en el cbuffer de camara ya escrito por el juego. Devuelve
// true si toco algo. `f` es el mapeo vivo: se escribe antes del Unmap real.
static bool tt_restaurar_m00(float *f, unsigned nfloats)
{
	if (nfloats < 52 || g_tt_inl_m00_ancho <= 0.0f)
		return false;
	float m00 = f[0], m11 = f[5];
	// Solo las camaras que ensanchamos: su m00 es exactamente el que pusimos.
	if (fabsf(m00 - g_tt_inl_m00_ancho) > 1e-6f * g_tt_inl_m00_ancho)
		return false;
	float orig = tt_m00_buscar(m11);
	if (orig <= 0.0f) {
		InterlockedIncrement(&g_tt_m00_sin_par);
		return false;
	}
	float k = orig / m00;             // > 1: el ancho es MAS chico que el original
	f[0] = orig;                      // projection m00
	if (nfloats > 16 && f[16] != 0.0f)
		f[16] /= k;                   // inverseProj [0][0] = 1/m00
	f[36] *= k;                       // relativeViewProjection: la componente .x
	f[40] *= k;
	f[44] *= k;
	f[48] *= k;
	InterlockedIncrement(&g_tt_m00_restauradas);
	return true;
}

// 1 = build de DIAGNOSTICO (hook de la vtable, TTPERSP/TTOVR, vigilancia del fov con
// breakpoints de hardware). 0 = producto: solo TTEYE/TTINL. Ver tt_cam_init.
#define TT_CULL_DIAG        0
#define TT_CULL_ASPECT_MIN  3.6f     // aspecto "de triple": espejos 3.0 y envmap 1.0 quedan fuera
#define TT_CULL_ASPECT_MAX  12.0f    // por encima de esto el aspecto es NUESTRO
#define TT_CULL_FOV_MAX     100.0f   // con fov >= 100 el objetivo cae debajo de 12: no se toca

static volatile LONG g_tt_cam_widen = 0;           // copia de $cull_widen (0/1), la lee tt_cam_update
static volatile LONG g_tt_cam_widen_val = 0;       // el valor crudo de $cull_widen (0, 8 o 1)
static CommandListVariable *g_tt_cam_ini_widen = NULL;
static float g_tt_cam_aspect_orig = 0.0f;          // ultimo aspecto original de triple visto
static unsigned g_tt_cam_ensanchadas = 0, g_tt_cam_restauradas = 0, g_tt_cam_llamadas = 0;

// Una entrada por (sitio de llamada, persp, fov, aspecto): asi la tabla no se
// desborda aunque el juego cree camaras nuevas todo el tiempo.
struct tt_cam_forma {
	void *ret;
	unsigned char persp;
	float fov, aspect, nearp, farp;   // aspect = el ORIGINAL (antes del ensanche)
	void *cam_ejemplo;
	DWORD hilo;
	unsigned llamadas, llamadas_prev, ensanchadas;
	unsigned frame_prev, frame_log;
};
#define TT_CAM_MAX 96
static tt_cam_forma g_tt_cams[TT_CAM_MAX];
static int g_tt_cams_n = 0;
static unsigned g_tt_cams_otras = 0;

static void tt_cam_log(const tt_cam_forma &v, const float *m, float aspect_final, const char *motivo, unsigned frame)
{
	unsigned frames = frame - v.frame_prev;
	float por_frame = frames ? (float)(v.llamadas - v.llamadas_prev) / (float)frames : 0.0f;
	float hfov = (v.persp && v.fov > 0.0f && v.fov < 180.0f)
		? 2.0f * atanf(aspect_final * tanf(v.fov * 0.5f * 0.017453292f)) * 57.29578f : 0.0f;
	LogInfo("TTCAM: %s ret=exe+0x%X persp=%d fov=%.3f aspect=%.4f->%.4f (hfov=%.1f) near=%.3f far=%.1f | "
		"m00=%.5f m11=%.5f m22=%.5f m23=%.4f | cam=%p hilo=%lu llamadas=%u (%.2f/frame) ensanchadas=%u frame=%u\n",
		motivo, (unsigned)((unsigned char *)v.ret - g_tt_cam_exe), (int)v.persp, v.fov, v.aspect, aspect_final, hfov,
		v.nearp, v.farp, m[0], m[5], m[10], m[14],
		v.cam_ejemplo, (unsigned long)v.hilo, v.llamadas, por_frame, v.ensanchadas, frame);
}

static void __fastcall tt_cam_build_hook(void *self, void *edx_sin_uso)
{
	void *ret = _ReturnAddress();
	unsigned char *cam = (unsigned char *)self;
	unsigned char persp = 0;
	float fov = 0, aspect = 0, nearp = 0, farp = 0;
	bool leido = false;
	__try {
		persp  = cam[TT_OFF_CAM_PERSP];
		fov    = *(float *)(cam + TT_OFF_CAM_FOV);
		nearp  = *(float *)(cam + TT_OFF_CAM_NEAR);
		farp   = *(float *)(cam + TT_OFF_CAM_FAR);
		aspect = *(float *)(cam + TT_OFF_CAM_ASPECT);
		leido = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	// --- el ensanche (solo perspectivas con aspecto de triple) ---
	float aspect_orig = aspect, aspect_final = aspect;
	bool ensanchada = false, restaurada = false;
	if (leido && persp && fov > 1.0f && fov < TT_CULL_FOV_MAX) {
		bool original = aspect > TT_CULL_ASPECT_MIN && aspect < TT_CULL_ASPECT_MAX;
		bool nuestro  = aspect >= TT_CULL_ASPECT_MAX && g_tt_cam_aspect_orig > 0.0f;
		if (original)
			g_tt_cam_aspect_orig = aspect;
		else if (nuestro)
			aspect_orig = g_tt_cam_aspect_orig;
		// BUILD 3: el ensanche por la vtable se APAGO. La corrida 2 mostro que
		// aplasta la UI (las camaras de UI tambien pasan por aca) y no toca el
		// descarte: la camara de escena recibe su proyeccion de otro lado.
		// BUILD 6: vuelve, pero SOLO para los cuatro setters de la camara de
		// escena del armado del frame (ret en exe+0x5D6F40..0x5D6FA0) y SOLO en
		// el tercer estado de la tecla ($cull_widen = 1). Asi la camara de escena
		// queda con el aspecto ancho tambien en sus CAMPOS (+0xF0), para los
		// sistemas que leen campos y no la matriz (terreno, LOD...). La UI no
		// pasa por esos sitios. Estado 8 = solo la matriz (TTEYE/TTINL).
		// BUILD 9: RETIRADO. La corrida 8 lo midio: con los campos anchos no
		// aparece ni un arbusto ni un trozo de terreno mas que con la matriz
		// sola, y cambia la oclusion ambiental (HDAO lee tan(fov/2)*aspecto de
		// estos campos). Queda `widen = false` y el codigo, por si hay que
		// volver a medirlo; la tecla ya no tiene tercer estado.
		unsigned ret_rva = (unsigned)((unsigned char *)ret - g_tt_cam_exe);
		bool sitio_escena = ret_rva >= 0x5D6F40u && ret_rva < 0x5D6FA0u;
		bool widen = false && sitio_escena;
		if (widen && (original || nuestro)) {
			float t = tanf(fov * 0.5f * 0.017453292f);
			float objetivo = tanf(TT_CULL_HFOV * 0.5f * 0.017453292f) / t;
			if (objetivo > aspect_orig) {
				__try {
					*(float *)(cam + TT_OFF_CAM_ASPECT) = objetivo;
					aspect_final = objetivo;
					ensanchada = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
		} else if (!widen && nuestro) {
			__try {
				*(float *)(cam + TT_OFF_CAM_ASPECT) = g_tt_cam_aspect_orig;
				aspect_final = g_tt_cam_aspect_orig;
				restaurada = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}

	g_tt_cam_build_orig(self, edx_sin_uso);

	float m[16] = { 0 };
	__try {
		memcpy(m, cam + TT_OFF_CAM_PROJ, sizeof(m));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}

	unsigned frame = G ? G->frame_no : 0;
	EnterCriticalSection(&g_tt_cam_cs);
	g_tt_cam_llamadas++;
	if (ensanchada) g_tt_cam_ensanchadas++;
	if (restaurada) g_tt_cam_restauradas++;
	int i;
	for (i = 0; i < g_tt_cams_n; i++) {
		const tt_cam_forma &e = g_tt_cams[i];
		if (e.ret == ret && e.persp == persp && fabsf(e.fov - fov) < 0.05f && fabsf(e.aspect - aspect_orig) < 0.005f)
			break;
	}
	if (i == g_tt_cams_n) {
		if (g_tt_cams_n >= TT_CAM_MAX) {
			g_tt_cams_otras++;
			LeaveCriticalSection(&g_tt_cam_cs);
			return;
		}
		tt_cam_forma &v = g_tt_cams[g_tt_cams_n++];
		memset(&v, 0, sizeof(v));
		v.ret = ret; v.persp = persp; v.fov = fov; v.aspect = aspect_orig; v.nearp = nearp; v.farp = farp;
		v.cam_ejemplo = self; v.hilo = GetCurrentThreadId(); v.llamadas = 1; v.ensanchadas = ensanchada ? 1 : 0;
		v.frame_prev = frame; v.frame_log = frame;
		tt_cam_log(v, m, aspect_final, "NUEVA ", frame);
		LeaveCriticalSection(&g_tt_cam_cs);
		return;
	}
	tt_cam_forma &v = g_tt_cams[i];
	v.llamadas++;
	if (ensanchada) v.ensanchadas++;
	v.nearp = nearp; v.farp = farp; v.cam_ejemplo = self; v.hilo = GetCurrentThreadId();
	if (frame - v.frame_log >= 600) {
		tt_cam_log(v, m, aspect_final, "LATIDO", frame);
		v.frame_log = frame;
		v.llamadas_prev = v.llamadas;
		v.frame_prev = frame;
	}
	LeaveCriticalSection(&g_tt_cam_cs);
}

// ===== BUILD 3: la proyeccion de la ESCENA no sale de la vtable ==================
//
// Lo que dijo la corrida 2 (ensanche por la vtable, tecla B): las aplicaciones
// corrian (34 por frame), la UI se aplastaba (prueba de que la matriz ensanchada
// llegaba a los shaders de UI), y el descarte NO cambio ni un arbol. El
// desensamblado del armado del frame (exe+0x5D43D0) explica las dos cosas: la
// camara de escena pasa por los cuatro setters inlineados (fov, aspecto, near,
// far: cada uno escribe el campo y llama al slot 16), pero JUSTO DESPUES
// (exe+0x5D6FC3) le pisa +0x110 con una matriz que armo aparte:
//
//   exe+0x5D6F0B: call exe+0x5CF150(out, near, far, fov_RADIANES, aspecto,
//                                   corrimiento_x, corrimiento_y)
//
// exe+0x5CF150 es un constructor de perspectiva suelto (cot(fov/2), cot/aspecto,
// (n+f)/(n-f), 2nf/(n-f), mas un corrimiento oblicuo en m20/m30). Tiene UN solo
// llamador. Su salida va a camara+0x110, y con la vista (+0x150) se multiplica en
// camara+0x190 (view*proj). ESA es la proyeccion de la escena: la que va a cb3 y,
// casi seguro, de la que el motor saca los planos de descarte.
//
// El ensanche se muda ahi: se cambia el rel32 del `call` (4 bytes en .text, con
// firma del sitio y del destino) para que llame a tt_persp_hook, que ajusta el
// ASPECTO del argumento y llama a la original. La UI no pasa por aca, asi que ya
// no se aplasta. Se restaura al descargar el dll.

#define TT_RVA_PERSP_CALL   0x5D6F0Bu   // el E8 del call
#define TT_RVA_PERSP_FN     0x5CF150u   // el constructor de perspectiva suelto
#define TT_RVA_PERSP_SITIO  0x5D6EFAu   // firma alrededor del call

typedef float *(__cdecl *tt_persp_fn)(float *out, float nearp, float farp, float fov_rad, float aspect, float sx, float sy);
static tt_persp_fn g_tt_persp_orig = NULL;
static unsigned char *g_tt_persp_call = NULL;     // direccion del E8
static volatile LONG g_tt_persp_state = 0;         // 1 instalado, -1 inerte
static unsigned g_tt_persp_llamadas = 0, g_tt_persp_ensanchadas = 0;

struct tt_persp_forma {
	float nearp, farp, fov_rad, aspect, sx, sy;
	unsigned llamadas, llamadas_prev, ensanchadas, frame_prev, frame_log;
};
#define TT_PERSP_MAX 32
static tt_persp_forma g_tt_persp[TT_PERSP_MAX];
static int g_tt_persp_n = 0;

static void tt_persp_log(const tt_persp_forma &v, float aspect_final, const float *m, const char *motivo, unsigned frame)
{
	unsigned frames = frame - v.frame_prev;
	float por_frame = frames ? (float)(v.llamadas - v.llamadas_prev) / (float)frames : 0.0f;
	float fov_deg = v.fov_rad * 57.29578f;
	float hfov = 2.0f * atanf(aspect_final * tanf(v.fov_rad * 0.5f)) * 57.29578f;
	LogInfo("TTPERSP: %s fov=%.3f aspect=%.4f->%.4f (hfov=%.1f) near=%.3f far=%.1f sx=%.4f sy=%.4f | "
		"m00=%.5f m11=%.5f m22=%.5f m23=%.4f m20=%.4f m30=%.4f | llamadas=%u (%.2f/frame) ensanchadas=%u frame=%u\n",
		motivo, fov_deg, v.aspect, aspect_final, hfov, v.nearp, v.farp, v.sx, v.sy,
		m[0], m[5], m[10], m[14], m[8], m[12], v.llamadas, por_frame, v.ensanchadas, frame);
}

static float *__cdecl tt_persp_hook(float *out, float nearp, float farp, float fov_rad, float aspect, float sx, float sy)
{
	float a = aspect;
	bool ensanchada = false;
	bool widen = InterlockedCompareExchange(&g_tt_cam_widen, 0, 0) != 0;
	if (widen && fov_rad > 0.02f && fov_rad < TT_CULL_FOV_MAX * 0.017453292f
	    && aspect > TT_CULL_ASPECT_MIN && aspect < TT_CULL_ASPECT_MAX) {
		float objetivo = tanf(TT_CULL_HFOV * 0.5f * 0.017453292f) / tanf(fov_rad * 0.5f);
		if (objetivo > aspect) {
			a = objetivo;
			ensanchada = true;
		}
	}
	float *r = g_tt_persp_orig(out, nearp, farp, fov_rad, a, sx, sy);

	float m[16] = { 0 };
	__try {
		if (r) memcpy(m, r, sizeof(m));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	unsigned frame = G ? G->frame_no : 0;
	EnterCriticalSection(&g_tt_cam_cs);
	g_tt_persp_llamadas++;
	if (ensanchada) g_tt_persp_ensanchadas++;
	int i;
	for (i = 0; i < g_tt_persp_n; i++) {
		const tt_persp_forma &e = g_tt_persp[i];
		if (fabsf(e.fov_rad - fov_rad) < 0.001f && fabsf(e.aspect - aspect) < 0.005f
		    && fabsf(e.nearp - nearp) < 0.001f && fabsf(e.farp - farp) < 0.5f)
			break;
	}
	if (i == g_tt_persp_n && g_tt_persp_n < TT_PERSP_MAX) {
		tt_persp_forma &v = g_tt_persp[g_tt_persp_n++];
		memset(&v, 0, sizeof(v));
		v.nearp = nearp; v.farp = farp; v.fov_rad = fov_rad; v.aspect = aspect; v.sx = sx; v.sy = sy;
		v.llamadas = 1; v.ensanchadas = ensanchada ? 1 : 0; v.frame_prev = frame; v.frame_log = frame;
		tt_persp_log(v, a, m, "NUEVA ", frame);
	} else if (i < g_tt_persp_n) {
		tt_persp_forma &v = g_tt_persp[i];
		v.llamadas++;
		if (ensanchada) v.ensanchadas++;
		v.sx = sx; v.sy = sy;
		if (frame - v.frame_log >= 600) {
			tt_persp_log(v, a, m, "LATIDO", frame);
			v.frame_log = frame; v.llamadas_prev = v.llamadas; v.frame_prev = frame;
		}
	}
	LeaveCriticalSection(&g_tt_cam_cs);
	return r;
}

static bool tt_persp_escribir_rel32(unsigned char *call, void *destino)
{
	DWORD viejo = 0;
	if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &viejo))
		return false;
	LONG rel = (LONG)((uintptr_t)destino - (uintptr_t)(call + 5));
	InterlockedExchange((volatile LONG *)(call + 1), rel);
	VirtualProtect(call + 1, 4, viejo, &viejo);
	FlushInstructionCache(GetCurrentProcess(), call, 5);
	return true;
}

static bool tt_persp_init(unsigned char *exe)
{
	// firma del sitio (exe+0x5D6EFA): movss xmm0,[ecx+0x40]; lea edx,[ebp-0x13d0];
	// movss [esp],xmm0; push edx; call REL32; add esp,0x1c; mov ecx,0x10; mov esi,eax; ...
	static const unsigned char s_sitio[] = {
		0xF3,0x0F,0x10,0x41,0x40, 0x8D,0x95,0x30,0xEC,0xFF,0xFF, 0xF3,0x0F,0x11,0x04,0x24, 0x52,
		0xE8,0,0,0,0, 0x83,0xC4,0x1C, 0xB9,0x10,0x00,0x00,0x00, 0x8B,0xF0, 0x8D,0xBD,0x30,0xFE,0xFF,0xFF, 0xF3,0xA5 };
	static const unsigned char m_sitio[sizeof(s_sitio)] = {
		1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1,1, 1,
		1,0,0,0,0, 1,1,1, 1,1,1,1,1, 1,1, 1,1,1,1,1,1, 1,1 };
	// firma del destino (exe+0x5CF150): prologo + movss xmm1,[ebx+0x14]; mulss xmm1,[0.5];
	// movss xmm0,[pi/2]; subss xmm0,xmm1; call tanf; movss xmm1,[ebx+0xC]
	static const unsigned char s_fn[] = {
		0x53,0x8B,0xDC,0x83,0xEC,0x08,0x83,0xE4,0xF0,0x83,0xC4,0x04,0x55,0x8B,0x6B,0x04,
		0x89,0x6C,0x24,0x04,0x8B,0xEC,0x83,0xEC,0x30, 0xF3,0x0F,0x10,0x4B,0x14, 0xF3,0x0F,0x59,0x0D,0,0,0,0,
		0xF3,0x0F,0x10,0x05,0,0,0,0, 0xF3,0x0F,0x5C,0xC1, 0xE8,0,0,0,0, 0xF3,0x0F,0x10,0x4B,0x0C };
	static const unsigned char m_fn[sizeof(s_fn)] = {
		1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
		1,1,1,1,1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,0,0,0,0,
		1,1,1,1,0,0,0,0, 1,1,1,1, 1,0,0,0,0, 1,1,1,1,1 };

	unsigned char *call = exe + TT_RVA_PERSP_CALL;
	bool ok = false;
	__try {
		LONG rel = *(LONG *)(call + 1);
		ok = tt_firma(exe, TT_RVA_PERSP_SITIO, s_sitio, m_sitio, sizeof(s_sitio))
		  && tt_firma(exe, TT_RVA_PERSP_FN, s_fn, m_fn, sizeof(s_fn))
		  && (call + 5 + rel) == (exe + TT_RVA_PERSP_FN);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTPERSP: las firmas del constructor de perspectiva no coinciden; ensanche inerte (fail-open).\n");
		InterlockedExchange(&g_tt_persp_state, -1);
		return false;
	}
	g_tt_persp_orig = (tt_persp_fn)(exe + TT_RVA_PERSP_FN);
	g_tt_persp_call = call;
	if (!tt_persp_escribir_rel32(call, (void *)&tt_persp_hook)) {
		LogInfo("TTPERSP: VirtualProtect fallo (%lu); ensanche inerte.\n", GetLastError());
		InterlockedExchange(&g_tt_persp_state, -1);
		return false;
	}
	InterlockedExchange(&g_tt_persp_state, 1);
	LogInfo("TTPERSP: call de exe+0x%X redirigido de exe+0x%X a %p (constructor de perspectiva de la escena).\n",
		TT_RVA_PERSP_CALL, TT_RVA_PERSP_FN, &tt_persp_hook);
	return true;
}

// ===== BUILD 4: la escena NO pasa por exe+0x5CF150; pasa por LibOVR ==============
//
// Corrida 3: TTPERSP instalado y CERO llamadas en toda la sesion. El armado del
// frame tiene dos ramas para la proyeccion de la escena, y el juego toma la OTRA:
// cuando el renderer (global exe+0xEA3B30) tiene [+0x569C] != 0 y [+0x57D8] == 1
// (chequeo exe+0x138160: el camino "oculus" de hardware_settings_config.xml, que
// esta prendido de fabrica aunque no haya visor), la proyeccion la arma
// ovrMatrix4f_Projection de LibOVR, enlazada estaticamente en el exe:
//
//   exe+0x5D6C0A: call exe+0xACC530(out, FovPort{up,down,left,right} (tangentes),
//                                   near, far, flags)     <- un solo llamador
//
// Las tangentes salen del descriptor de ojo 0 del renderer (+0x5808: +4 up, +8
// down, +0xC left, +0x10 right). El resultado va a camara+0x110 y a +0x190 con la
// vista, igual que en la otra rama. La sub-proyeccion ortografica del HUD
// (exe+0xACC410) usa OTROS campos del descriptor (+0x24..+0x2C): la UI no se toca.
//
// El ensanche: se redirige ese call y se reemplazan las tangentes left/right por
// tan(TT_CULL_HFOV/2). Con la tecla en 1 (tercer estado del cycle), ADEMAS se
// escriben en el descriptor, para el lector exe+0x60FA20 que calcula un FOV desde
// [+0x5814]+[+0x5818] (candidato a alimentar el descarte o el LOD). Se mide.

#define TT_RVA_OVR_CALL     0x5D6C0Au
#define TT_RVA_OVR_FN       0xACC530u
#define TT_RVA_OVR_SITIO    0x5D6BD5u
#define TT_RVA_RENDERER     0xEA3B30u   // global: puntero al renderer
#define TT_OFF_RD_OCULUS    0x569C
#define TT_OFF_RD_MODO      0x57D8
#define TT_OFF_RD_EYE0      0x5808
#define TT_OFF_EYE_LEFT     0x0C
#define TT_OFF_EYE_RIGHT    0x10

typedef float *(__cdecl *tt_ovr_fn)(float *out, float up, float down, float left, float right, float nearp, float farp, unsigned flags);
static tt_ovr_fn g_tt_ovr_orig = NULL;
static unsigned char *g_tt_ovr_call = NULL;
static volatile LONG g_tt_ovr_state = 0;
static unsigned g_tt_ovr_llamadas = 0, g_tt_ovr_ensanchadas = 0, g_tt_ovr_desc = 0;

struct tt_ovr_forma {
	float up, down, left, right, nearp, farp; unsigned flags;
	unsigned llamadas, llamadas_prev, ensanchadas, frame_prev, frame_log;
};
static tt_ovr_forma g_tt_ovr[TT_PERSP_MAX];
static int g_tt_ovr_n = 0;

static void tt_ovr_log(const tt_ovr_forma &v, float l, float r, const float *m, const char *motivo, unsigned frame)
{
	unsigned frames = frame - v.frame_prev;
	float por_frame = frames ? (float)(v.llamadas - v.llamadas_prev) / (float)frames : 0.0f;
	float hfov = (atanf(l) + atanf(r)) * 57.29578f;
	float vfov = (atanf(v.up) + atanf(v.down)) * 57.29578f;
	LogInfo("TTOVR: %s up=%.4f down=%.4f left=%.4f right=%.4f (vfov=%.1f) -> left=%.3f right=%.3f (hfov=%.1f) near=%.3f far=%.1f flags=%u | "
		"m00=%.5f m11=%.5f m22=%.5f m23=%.4f m20=%.4f m21=%.4f | llamadas=%u (%.2f/frame) ensanchadas=%u frame=%u\n",
		motivo, v.up, v.down, v.left, v.right, vfov, l, r, hfov, v.nearp, v.farp, v.flags,
		m[0], m[5], m[10], m[14], m[8], m[9], v.llamadas, por_frame, v.ensanchadas, frame);
}

static float *__cdecl tt_ovr_hook(float *out, float up, float down, float left, float right, float nearp, float farp, unsigned flags)
{
	float l = left, r = right;
	bool ensanchada = false;
	LONG val = InterlockedCompareExchange(&g_tt_cam_widen_val, 0, 0);
	if (val != 0 && left + right > 3.0f && left < TT_CULL_ASPECT_MAX && right < TT_CULL_ASPECT_MAX) {
		float t = tanf(TT_CULL_HFOV * 0.5f * 0.017453292f);
		l = r = t;
		ensanchada = true;
		if (val == 1) {
			// tercer estado de la tecla: tambien el descriptor del ojo 0
			__try {
				unsigned char *rd = *(unsigned char **)(g_tt_cam_exe + TT_RVA_RENDERER);
				if ((uintptr_t)rd >= 0x10000) {
					*(float *)(rd + TT_OFF_RD_EYE0 + TT_OFF_EYE_LEFT) = t;
					*(float *)(rd + TT_OFF_RD_EYE0 + TT_OFF_EYE_RIGHT) = t;
					g_tt_ovr_desc++;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}
	float *res = g_tt_ovr_orig(out, up, down, l, r, nearp, farp, flags);

	float m[16] = { 0 };
	__try {
		if (res) memcpy(m, res, sizeof(m));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	unsigned frame = G ? G->frame_no : 0;
	EnterCriticalSection(&g_tt_cam_cs);
	g_tt_ovr_llamadas++;
	if (ensanchada) g_tt_ovr_ensanchadas++;
	int i;
	for (i = 0; i < g_tt_ovr_n; i++) {
		const tt_ovr_forma &e = g_tt_ovr[i];
		if (fabsf(e.up - up) < 0.002f && fabsf(e.down - down) < 0.002f && fabsf(e.left - left) < 0.002f
		    && fabsf(e.right - right) < 0.002f && fabsf(e.nearp - nearp) < 0.001f && fabsf(e.farp - farp) < 0.5f && e.flags == flags)
			break;
	}
	if (i == g_tt_ovr_n && g_tt_ovr_n < TT_PERSP_MAX) {
		tt_ovr_forma &v = g_tt_ovr[g_tt_ovr_n++];
		memset(&v, 0, sizeof(v));
		v.up = up; v.down = down; v.left = left; v.right = right; v.nearp = nearp; v.farp = farp; v.flags = flags;
		v.llamadas = 1; v.ensanchadas = ensanchada ? 1 : 0; v.frame_prev = frame; v.frame_log = frame;
		tt_ovr_log(v, l, r, m, "NUEVA ", frame);
	} else if (i < g_tt_ovr_n) {
		tt_ovr_forma &v = g_tt_ovr[i];
		v.llamadas++;
		if (ensanchada) v.ensanchadas++;
		if (frame - v.frame_log >= 600) {
			tt_ovr_log(v, l, r, m, "LATIDO", frame);
			v.frame_log = frame; v.llamadas_prev = v.llamadas; v.frame_prev = frame;
		}
	}
	LeaveCriticalSection(&g_tt_cam_cs);
	return res;
}

static bool tt_ovr_init(unsigned char *exe)
{
	// sitio (exe+0x5D6BD5): push 0; sub esp,0x18; movss [esp+0x14],xmm0; mov [ebp-0x54],eax;
	// movss xmm0,[edx+0x40]; movss [esp+0x10],xmm0; movq xmm0,[eax+4]; mov ecx,esp; movq [ecx],xmm0;
	// movq xmm0,[eax+0xC]; lea eax,[ebp-0x1390]; push eax; movq [ecx+8],xmm0; call REL32; mov edx,[ebp-0x34]
	static const unsigned char s_sitio[] = {
		0x6A,0x00, 0x83,0xEC,0x18, 0xF3,0x0F,0x11,0x44,0x24,0x14, 0x89,0x45,0xAC, 0xF3,0x0F,0x10,0x42,0x40,
		0xF3,0x0F,0x11,0x44,0x24,0x10, 0xF3,0x0F,0x7E,0x40,0x04, 0x8B,0xCC, 0x66,0x0F,0xD6,0x01,
		0xF3,0x0F,0x7E,0x40,0x0C, 0x8D,0x85,0x70,0xEC,0xFF,0xFF, 0x50, 0x66,0x0F,0xD6,0x41,0x08,
		0xE8,0,0,0,0, 0x8B,0x55,0xCC };
	static const unsigned char m_sitio[sizeof(s_sitio)] = {
		1,1, 1,1,1, 1,1,1,1,1,1, 1,1,1, 1,1,1,1,1,
		1,1,1,1,1,1, 1,1,1,1,1, 1,1, 1,1,1,1,
		1,1,1,1,1, 1,1,1,1,1,1, 1, 1,1,1,1,1,
		1,0,0,0,0, 1,1,1 };
	// destino (exe+0xACC530): push ebp; mov ebp,esp; sub esp,0x5C; mov eax,[cookie]; xor eax,ebp;
	// mov [ebp-4],eax; mov al,[ebp+0x24]; fld [ebp+0x1C]; test al,1; fstp [ebp-0x48]; fld [ebp+0x20]
	static const unsigned char s_fn[] = {
		0x55,0x8B,0xEC,0x83,0xEC,0x5C, 0xA1,0,0,0,0, 0x33,0xC5, 0x89,0x45,0xFC, 0x8A,0x45,0x24,
		0xD9,0x45,0x1C, 0xA8,0x01, 0xD9,0x5D,0xB8, 0xD9,0x45,0x20 };
	static const unsigned char m_fn[sizeof(s_fn)] = {
		1,1,1,1,1,1, 1,0,0,0,0, 1,1, 1,1,1, 1,1,1,
		1,1,1, 1,1, 1,1,1, 1,1,1 };
	// el lector del renderer (exe+0x60FA5B): mov esi,[global]; mov [ebp-0x58],esi
	static const unsigned char s_rd[] = { 0x8B,0x35,0,0,0,0, 0x89,0x75,0xA8 };
	static const unsigned char m_rd[] = { 1,1,0,0,0,0, 1,1,1 };

	unsigned char *call = exe + TT_RVA_OVR_CALL;
	bool ok = false;
	__try {
		LONG rel = *(LONG *)(call + 1);
		ok = tt_firma(exe, TT_RVA_OVR_SITIO, s_sitio, m_sitio, sizeof(s_sitio))
		  && tt_firma(exe, TT_RVA_OVR_FN, s_fn, m_fn, sizeof(s_fn))
		  && (call + 5 + rel) == (exe + TT_RVA_OVR_FN)
		  && tt_firma(exe, 0x60FA5B, s_rd, m_rd, sizeof(s_rd))
		  && tt_dword_es(exe, 0x60FA5B + 2, exe + TT_RVA_RENDERER);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTOVR: las firmas de ovrMatrix4f_Projection no coinciden; ensanche inerte (fail-open).\n");
		InterlockedExchange(&g_tt_ovr_state, -1);
		return false;
	}
	g_tt_ovr_orig = (tt_ovr_fn)(exe + TT_RVA_OVR_FN);
	g_tt_ovr_call = call;
	if (!tt_persp_escribir_rel32(call, (void *)&tt_ovr_hook)) {
		LogInfo("TTOVR: VirtualProtect fallo (%lu); ensanche inerte.\n", GetLastError());
		InterlockedExchange(&g_tt_ovr_state, -1);
		return false;
	}
	InterlockedExchange(&g_tt_ovr_state, 1);
	__try {
		unsigned char *rd = *(unsigned char **)(exe + TT_RVA_RENDERER);
		LogInfo("TTOVR: call de exe+0x%X redirigido de exe+0x%X a %p (ovrMatrix4f_Projection). renderer=%p oculus=%d modo=%d\n",
			TT_RVA_OVR_CALL, TT_RVA_OVR_FN, &tt_ovr_hook, rd,
			((uintptr_t)rd >= 0x10000) ? *(int *)(rd + TT_OFF_RD_OCULUS) : -1,
			((uintptr_t)rd >= 0x10000) ? *(int *)(rd + TT_OFF_RD_MODO) : -1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return true;
}

// ===== BUILD 5: la escena usa la TERCERA rama, la inline ==========================
//
// Corrida 4: TTPERSP y TTOVR instalados, cero llamadas los dos; el log del hook
// dijo renderer oculus=0 modo=1, o sea exe+0x138160 devuelve falso y la rama LibOVR
// no corre. La rama que corre es la de exe+0x5D713B..0x5D72A2: si el objeto de
// parametros de camara tiene [+0x145] != 0 llama a exe+0x8C56C0(out, fov_rad,
// aspecto, near, far) (una variante que consulta exe+0x8C53F0: "3 pantallas, modo
// 2" = el Eyefinity del propio juego), y si no, ARMA LA PERSPECTIVA INLINE:
//
//   exe+0x5D71D7  cot = tan(pi/2 - fov/2)
//   exe+0x5D7221  divss xmm5, [ebp-0x64]      <- m00 = cot / aspecto
//   ...           m11 = cot, m22 = (n+f)/(n-f), m23 = 2nf/(n-f)  -> [ebp-0x1D0]
//   exe+0x5D72A2  jmp exe+0x5D6F25            -> camara+0x110, +0x190 = view*proj
//
// Las lineas `TRIPLE camera` del mod (m22 = -1.00002 con far 20000, no -1 exacto)
// confirman que la escena sale de esta rama, no de una con far infinito.
//
// El ensanche: el `divss xmm5,[ebp-0x64]` (5 bytes) se reemplaza por un `call` a un
// stub desnudo que hace lo mismo, salvo con la tecla prendida y aspecto 3.6..12: ahi
// pone xmm5 = 1/tan(TT_CULL_HFOV/2) directamente, que es cot/aspecto' con
// aspecto' = tan(hfov/2)/tan(fov/2) (no hace falta el fov: se simplifica). El stub
// no toca la pila del llamador (ebp sigue siendo el del frame) y solo usa eax, xmm5
// y xmm6 (muerto en ese punto: lo pisa exe+0x5D722A). La rama de exe+0x8C56C0 se
// cubre redirigiendo su call (mismo criterio sobre el argumento aspecto).

#define TT_RVA_INL_DIVSS    0x5D7221u   // divss xmm5, [ebp-0x64]
#define TT_RVA_INL_SITIO    0x5D7204u
#define TT_RVA_EYE_CALL     0x5D71CAu   // call exe+0x8C56C0
#define TT_RVA_EYE_FN       0x8C56C0u
#define TT_RVA_EYE_SITIO    0x5D719Au

static float g_tt_inl_m00 = 0.0f;              // 1/tan(TT_CULL_HFOV/2), lo llena tt_inl_init
static float g_tt_inl_aspect_min = TT_CULL_ASPECT_MIN;
static float g_tt_inl_aspect_max = TT_CULL_ASPECT_MAX;
static volatile LONG g_tt_inl_llamadas = 0, g_tt_inl_ensanchadas = 0;
static volatile LONG g_tt_inl_state = 0;
static unsigned char *g_tt_inl_sitio = NULL;
static unsigned char g_tt_inl_orig[5] = { 0 };

// Registra el par (m11, m00 original) de la rama inline. Se llama desde el stub,
// que en ese punto tiene el cot en xmm5 y el aspecto en [ebp-0x64].
static void __cdecl tt_inl_registrar(float cot, float aspecto)
{
	if (aspecto > 1e-6f)
		tt_m00_registrar(cot, cot / aspecto);
}

__declspec(naked) static void tt_inl_stub()
{
	__asm {
		movss   xmm6, dword ptr [ebp - 0x64]
		mov     eax, dword ptr [g_tt_cam_widen]
		test    eax, eax
		jz      original
		comiss  xmm6, dword ptr [g_tt_inl_aspect_min]
		jbe     original
		comiss  xmm6, dword ptr [g_tt_inl_aspect_max]
		jae     original
		// El par para poder deshacer el ensanche en el cbuffer. Se preservan los
		// registros que el llamador sigue usando; xmm6 esta muerto (lo pisa
		// exe+0x5D722A) y xmm5 se recarga abajo.
		sub     esp, 8
		movss   dword ptr [esp + 4], xmm6
		movss   dword ptr [esp], xmm5
		pushfd
		push    eax
		push    ecx
		push    edx
		sub     esp, 64
		movups  xmmword ptr [esp], xmm0
		movups  xmmword ptr [esp + 16], xmm1
		movups  xmmword ptr [esp + 32], xmm2
		movups  xmmword ptr [esp + 48], xmm3
		call    tt_inl_registrar
		movups  xmm0, xmmword ptr [esp]
		movups  xmm1, xmmword ptr [esp + 16]
		movups  xmm2, xmmword ptr [esp + 32]
		movups  xmm3, xmmword ptr [esp + 48]
		add     esp, 64
		pop     edx
		pop     ecx
		pop     eax
		popfd
		add     esp, 8
		movss   xmm5, dword ptr [g_tt_inl_m00]
		lock inc dword ptr [g_tt_inl_ensanchadas]
		lock inc dword ptr [g_tt_inl_llamadas]
		ret
	original:
		divss   xmm5, xmm6
		lock inc dword ptr [g_tt_inl_llamadas]
		ret
	}
}

typedef float *(__cdecl *tt_eye_fn)(float *out, float fov_rad, float aspect, float nearp, float farp);
static tt_eye_fn g_tt_eye_orig = NULL;
static unsigned char *g_tt_eye_call = NULL;
static volatile LONG g_tt_eye_state = 0;
static unsigned g_tt_eye_llamadas = 0, g_tt_eye_ensanchadas = 0;

static float *__cdecl tt_eye_hook(float *out, float fov_rad, float aspect, float nearp, float farp)
{
	float a = aspect;
	bool widen = InterlockedCompareExchange(&g_tt_cam_widen, 0, 0) != 0;
	bool ens = false;
	if (widen && fov_rad > 0.02f && fov_rad < TT_CULL_FOV_MAX * 0.017453292f
	    && aspect > TT_CULL_ASPECT_MIN && aspect < TT_CULL_ASPECT_MAX) {
		float objetivo = tanf(TT_CULL_HFOV * 0.5f * 0.017453292f) / tanf(fov_rad * 0.5f);
		if (objetivo > aspect) { a = objetivo; ens = true; }
	}
	if (ens) {
		// El par (m11, m00 original) para poder deshacerlo en el cbuffer. m11 es
		// el cot, que el ensanche NO toca: por eso sirve de clave.
		float cot = 1.0f / tanf(fov_rad * 0.5f);
		tt_m00_registrar(cot, cot / aspect);
	}
	float *r = g_tt_eye_orig(out, fov_rad, a, nearp, farp);
	EnterCriticalSection(&g_tt_cam_cs);
	if (g_tt_eye_llamadas == 0)
		LogInfo("TTEYE: primera llamada: fov=%.3f aspect=%.4f->%.4f near=%.3f far=%.1f\n",
			fov_rad * 57.29578f, aspect, a, nearp, farp);
	g_tt_eye_llamadas++;
	if (ens) g_tt_eye_ensanchadas++;
	LeaveCriticalSection(&g_tt_cam_cs);
	return r;
}

static bool tt_inl_init(unsigned char *exe)
{
	// exe+0x5D7204: movss xmm3,[1.0]; movaps xmm2,xmm1; subss xmm2,xmm4; divss xmm3,xmm2;
	// movaps xmm2,[ebp-0x1e0]; movaps xmm5,xmm0; divss xmm5,[ebp-0x64]; movaps [ebp-0x60],xmm2;
	// movaps xmm6,xmm1; movss [ebp-0x60],xmm5; movaps xmm5,[ebp-0x60]
	static const unsigned char s_inl[] = {
		0xF3,0x0F,0x10,0x1D,0,0,0,0, 0x0F,0x28,0xD1, 0xF3,0x0F,0x5C,0xD4, 0xF3,0x0F,0x5E,0xDA,
		0x0F,0x28,0x95,0x20,0xFE,0xFF,0xFF, 0x0F,0x28,0xE8, 0xF3,0x0F,0x5E,0x6D,0x9C,
		0x0F,0x29,0x55,0xA0, 0x0F,0x28,0xF1, 0xF3,0x0F,0x11,0x6D,0xA0, 0x0F,0x28,0x6D,0xA0 };
	static const unsigned char m_inl[sizeof(s_inl)] = {
		1,1,1,1,0,0,0,0, 1,1,1, 1,1,1,1, 1,1,1,1,
		1,1,1,1,1,1,1, 1,1,1, 1,1,1,1,1,
		1,1,1,1, 1,1,1, 1,1,1,1,1, 1,1,1,1 };
	// exe+0x5D719A: el sitio del call a exe+0x8C56C0
	static const unsigned char s_eye[] = {
		0xF3,0x0F,0x10,0x43,0x10, 0x83,0xEC,0x10, 0xF3,0x0F,0x11,0x44,0x24,0x0C, 0xF3,0x0F,0x10,0x40,0x40,
		0xF3,0x0F,0x11,0x44,0x24,0x08, 0xF3,0x0F,0x10,0x45,0x9C, 0xF3,0x0F,0x11,0x44,0x24,0x04,
		0x8D,0x8D,0xB0,0xEB,0xFF,0xFF, 0xF3,0x0F,0x11,0x0C,0x24, 0x51, 0xE8,0,0,0,0, 0x83,0xC4,0x14 };
	static const unsigned char m_eye[sizeof(s_eye)] = {
		1,1,1,1,1, 1,1,1, 1,1,1,1,1,1, 1,1,1,1,1,
		1,1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1,1,
		1,1,1,1,1,1, 1,1,1,1,1, 1, 1,0,0,0,0, 1,1,1 };
	static const unsigned char s_eyefn[] = {
		0x53,0x8B,0xDC,0x83,0xEC,0x08,0x83,0xE4,0xF0,0x83,0xC4,0x04,0x55,0x8B,0x6B,0x04,
		0x89,0x6C,0x24,0x04,0x8B,0xEC,0x83,0xEC,0x78,0x56,0x57,0x8D,0x45,0xFF,0x50 };
	static const unsigned char m_eyefn[sizeof(s_eyefn)] = {
		1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };

	g_tt_inl_m00 = 1.0f / tanf(TT_CULL_HFOV * 0.5f * 0.017453292f);
	g_tt_inl_m00_ancho = g_tt_inl_m00;

	bool ok_inl = false, ok_eye = false;
	unsigned char *call = exe + TT_RVA_EYE_CALL;
	__try {
		ok_inl = tt_firma(exe, TT_RVA_INL_SITIO, s_inl, m_inl, sizeof(s_inl));
		LONG rel = *(LONG *)(call + 1);
		ok_eye = tt_firma(exe, TT_RVA_EYE_SITIO, s_eye, m_eye, sizeof(s_eye))
		      && tt_firma(exe, TT_RVA_EYE_FN, s_eyefn, m_eyefn, sizeof(s_eyefn))
		      && (call + 5 + rel) == (exe + TT_RVA_EYE_FN);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok_inl = ok_eye = false;
	}

	if (ok_inl) {
		unsigned char *sitio = exe + TT_RVA_INL_DIVSS;
		DWORD viejo = 0;
		if (VirtualProtect(sitio, 5, PAGE_EXECUTE_READWRITE, &viejo)) {
			memcpy(g_tt_inl_orig, sitio, 5);
			unsigned char parche[5];
			parche[0] = 0xE8;
			LONG rel = (LONG)((uintptr_t)&tt_inl_stub - (uintptr_t)(sitio + 5));
			memcpy(parche + 1, &rel, 4);
			memcpy(sitio, parche, 5);
			VirtualProtect(sitio, 5, viejo, &viejo);
			FlushInstructionCache(GetCurrentProcess(), sitio, 5);
			g_tt_inl_sitio = sitio;
			InterlockedExchange(&g_tt_inl_state, 1);
			LogInfo("TTINL: divss de exe+0x%X reemplazado por call al stub %p (m00 ancho = %.5f).\n",
				TT_RVA_INL_DIVSS, &tt_inl_stub, g_tt_inl_m00);
		} else {
			LogInfo("TTINL: VirtualProtect fallo (%lu); inerte.\n", GetLastError());
			InterlockedExchange(&g_tt_inl_state, -1);
		}
	} else {
		LogInfo("TTINL: la firma de la perspectiva inline no coincide; inerte (fail-open).\n");
		InterlockedExchange(&g_tt_inl_state, -1);
	}

	if (ok_eye) {
		g_tt_eye_orig = (tt_eye_fn)(exe + TT_RVA_EYE_FN);
		g_tt_eye_call = call;
		if (tt_persp_escribir_rel32(call, (void *)&tt_eye_hook)) {
			InterlockedExchange(&g_tt_eye_state, 1);
			LogInfo("TTEYE: call de exe+0x%X redirigido de exe+0x%X a %p.\n", TT_RVA_EYE_CALL, TT_RVA_EYE_FN, &tt_eye_hook);
		} else {
			InterlockedExchange(&g_tt_eye_state, -1);
		}
	} else {
		LogInfo("TTEYE: las firmas no coinciden; inerte (fail-open).\n");
		InterlockedExchange(&g_tt_eye_state, -1);
	}
	return ok_inl;
}

// ===== BUILD 13: quien lee el FOV de la camara del JUEGO (2026-09-04) =============
//
// La geometria del volcado 12 dijo que el terreno de menos de 1300 m se corta a
// +-62..68 grados = el frustum ORIGINAL del juego (fov vertical 49 con aspecto
// 5.39), mientras que el lejano ya llega a +-85 con la B. O sea: hay un descarte
// que no lee la matriz (TTEYE/TTINL) ni los campos de la camara de render
// (build 6/8), y que casi seguro parte del FOV crudo de la camara del JUEGO: el
// objeto que el armado del frame recibe como segundo argumento y del que lee
// `+0x50` (fov en RADIANES) en exe+0x5D6B33 y exe+0x5D6F28 (y `+0x40` = posicion).
//
// Dos cosas en una corrida:
//
//  1. MEDICION (siempre): las dos lecturas del frame se reemplazan por stubs que
//     hacen lo mismo y ademas guardan el puntero del objeto. Con el puntero se
//     arma un BREAKPOINT DE HARDWARE de lectura/escritura (DR0, 4 bytes) en
//     `+0x50` en TODOS los hilos del proceso (Toolhelp + SetThreadContext, se
//     re-arma cada 2 s para los hilos nuevos) y un manejador vectorizado que, en
//     cada disparo, anota el EIP del lector. La tabla sale en el resumen cada 600
//     frames como `exe+0xRVA n=...`. Los stubs propios se filtran por modulo.
//     Es una herramienta de depuracion: no queda en el producto.
//
//  2. EXPERIMENTO (tecla en 1): el stub escribe en `+0x50` un fov ANCHO
//     (TT_FOV_WIDE_RAD: 2*atan(tan(86)/5.389) = 138.7 grados verticales, que con
//     el aspecto de triple da 172 horizontales) y le devuelve al frame el valor
//     ORIGINAL, asi la camara de render (y por ella m11, el HDAO y el mod) no
//     cambian. Todo lector del campo fuera del frame (el culler del terreno, si
//     lo lee de ahi) ve el ancho. Si el terreno medio aparece con la tecla en 1,
//     el culler lee este campo y el arreglo es este. Si no, la tabla de lectores
//     dice donde mirar.
//     El valor ancho es una constante reconocible: si el stub lo ve en el campo
//     (el juego no lo reescribio) usa el original guardado.

#include <tlhelp32.h>

#define TT_RVA_FOV_LEE_A    0x5D6B33u   // movss xmm0, [edi+0x50]
#define TT_RVA_FOV_LEE_B    0x5D6F28u   // movss xmm0, [edx+0x50]
#define TT_OFF_GC_FOV       0x50
#define TT_FOV_WIDE_RAD     2.42117f    // 138.72 grados

static volatile LONG g_tt_fov_state = 0;
static unsigned char *g_tt_fov_sitio_a = NULL, *g_tt_fov_sitio_b = NULL;
static unsigned char g_tt_fov_orig_a[5], g_tt_fov_orig_b[5];
static unsigned char *volatile g_tt_gamecam = NULL;
static float g_tt_fov_orig = 0.0f;
static float g_tt_fov_out = 0.0f;          // lo que el stub devuelve en xmm0
static float g_tt_fov_wide = TT_FOV_WIDE_RAD;
static volatile LONG g_tt_fov_escrituras = 0, g_tt_fov_restauradas = 0, g_tt_fov_lecturas = 0;
static bool g_tt_gamecam_volcada = false;

// tabla de lectores (EIP -> cuenta), llenada desde el manejador de excepciones
struct tt_lector { DWORD eip; LONG n; };
#define TT_LECTORES_MAX 96
static tt_lector g_tt_lectores[TT_LECTORES_MAX];
static volatile LONG g_tt_lectores_n = 0, g_tt_lectores_otros = 0, g_tt_wp_disparos = 0;
static HMODULE g_tt_este_dll = NULL;
static volatile LONG g_tt_wp_armados = 0, g_tt_wp_hilos = 0;
static unsigned char *volatile g_tt_wp_dir = NULL;

// BUILD 14: la corrida 13 mostro (EIP exe+0x0DE52E en la tabla de lectores, y es
// una ESCRITURA: `fld [ecx+0x50]; fstp [eax+0x50]`) que el objeto que lee el frame
// es una COPIA que el motor rellena ~7 veces por frame desde otro objeto (el
// operador de copia exe+0x0DE4D0, `this` = destino, arg = fuente). Escribir en la
// copia duraba una fraccion de frame. El ensanche pasa al operador de copia
// (tt_fov_copia_c), que guarda el fov original POR OBJETO destino; los stubs del
// frame ya no escriben: solo devuelven el original de su objeto.
struct tt_fov_orig_de { unsigned char *cam; float fov; };
#define TT_FOV_ORIG_MAX 16
static tt_fov_orig_de g_tt_fov_origs[TT_FOV_ORIG_MAX];
static volatile LONG g_tt_fov_copias = 0, g_tt_fov_copias_anchas = 0;
static volatile LONG g_tt_fov_copia_state = 0;
static unsigned char *g_tt_fov_copia_sitio = NULL;
static unsigned char g_tt_fov_copia_orig[6];

static float tt_fov_orig_buscar(unsigned char *cam, float respaldo)
{
	for (int i = 0; i < TT_FOV_ORIG_MAX; i++)
		if (g_tt_fov_origs[i].cam == cam)
			return g_tt_fov_origs[i].fov;
	return respaldo;
}

static void tt_fov_orig_guardar(unsigned char *cam, float fov)
{
	int libre = -1;
	for (int i = 0; i < TT_FOV_ORIG_MAX; i++) {
		if (g_tt_fov_origs[i].cam == cam) { g_tt_fov_origs[i].fov = fov; return; }
		if (!g_tt_fov_origs[i].cam && libre < 0) libre = i;
	}
	if (libre >= 0) { g_tt_fov_origs[libre].cam = cam; g_tt_fov_origs[libre].fov = fov; }
}

static void __cdecl tt_gamecam_fov_c(unsigned char *cam)
{
	float v = 0.0f;
	__try {
		v = *(float *)(cam + TT_OFF_GC_FOV);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_tt_fov_out = g_tt_fov_orig;
		return;
	}
	g_tt_gamecam = cam;
	InterlockedIncrement(&g_tt_fov_lecturas);
	bool nuestro = fabsf(v - g_tt_fov_wide) < 1e-6f;
	float orig = nuestro ? tt_fov_orig_buscar(cam, g_tt_fov_orig) : v;
	if (!nuestro)
		g_tt_fov_orig = v;
	g_tt_fov_out = orig;
}

// El operador de copia: `fld [ecx+0x50]; fstp [eax+0x50]` (6 bytes) -> call + nop.
// eax = destino, ecx = fuente. Se copia el fov y, con la tecla en 1, si el fov
// esta en el rango de una camara de conduccion (0.3..1.3 rad = 17..75 grados;
// espejos 15 y envmap/UI 90 quedan fuera), el destino recibe el ANCHO y el
// original se guarda por objeto para los stubs del frame.
static void __cdecl tt_fov_copia_c(unsigned char *src, unsigned char *dst)
{
	__try {
		float v = *(float *)(src + TT_OFF_GC_FOV);
		*(float *)(dst + TT_OFF_GC_FOV) = v;
		InterlockedIncrement(&g_tt_fov_copias);
		bool era_ancho = fabsf(v - g_tt_fov_wide) < 1e-6f;
		if (era_ancho)
			v = tt_fov_orig_buscar(src, g_tt_fov_orig);
		tt_fov_orig_guardar(dst, v);
		LONG val = InterlockedCompareExchange(&g_tt_cam_widen_val, 0, 0);
		if (val == 1 && v > 0.3f && v < 1.3f) {
			*(float *)(dst + TT_OFF_GC_FOV) = g_tt_fov_wide;
			InterlockedIncrement(&g_tt_fov_copias_anchas);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
}

__declspec(naked) static void tt_fov_copia_stub()
{
	__asm { pushfd }
	__asm { push eax }
	__asm { push ecx }
	__asm { push edx }
	__asm { push eax }
	__asm { push ecx }
	__asm { call tt_fov_copia_c }
	__asm { add esp, 8 }
	__asm { pop edx }
	__asm { pop ecx }
	__asm { pop eax }
	__asm { popfd }
	__asm { ret }
}

// Los stubs: mismo efecto que `movss xmm0,[reg+0x50]`, mas la captura. Preservan
// eax/ecx/edx/flags y xmm1..xmm7 alrededor de la llamada C (el helper puede pisar
// cualquier xmm; en los dos sitios solo xmm0 es el resultado).
#define TT_FOV_STUB(NOMBRE, REG) \
__declspec(naked) static void NOMBRE() \
{ \
	__asm { pushfd } \
	__asm { push eax } \
	__asm { push ecx } \
	__asm { push edx } \
	__asm { sub esp, 112 } \
	__asm { movups xmmword ptr [esp], xmm1 } \
	__asm { movups xmmword ptr [esp+16], xmm2 } \
	__asm { movups xmmword ptr [esp+32], xmm3 } \
	__asm { movups xmmword ptr [esp+48], xmm4 } \
	__asm { movups xmmword ptr [esp+64], xmm5 } \
	__asm { movups xmmword ptr [esp+80], xmm6 } \
	__asm { movups xmmword ptr [esp+96], xmm7 } \
	__asm { push REG } \
	__asm { call tt_gamecam_fov_c } \
	__asm { add esp, 4 } \
	__asm { movups xmm1, xmmword ptr [esp] } \
	__asm { movups xmm2, xmmword ptr [esp+16] } \
	__asm { movups xmm3, xmmword ptr [esp+32] } \
	__asm { movups xmm4, xmmword ptr [esp+48] } \
	__asm { movups xmm5, xmmword ptr [esp+64] } \
	__asm { movups xmm6, xmmword ptr [esp+80] } \
	__asm { movups xmm7, xmmword ptr [esp+96] } \
	__asm { add esp, 112 } \
	__asm { pop edx } \
	__asm { pop ecx } \
	__asm { pop eax } \
	__asm { popfd } \
	__asm { movss xmm0, dword ptr [g_tt_fov_out] } \
	__asm { ret } \
}
TT_FOV_STUB(tt_fov_stub_edi, edi)
TT_FOV_STUB(tt_fov_stub_edx, edx)

static LONG CALLBACK tt_veh(EXCEPTION_POINTERS *e)
{
	if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT *c = e->ContextRecord;
	if (!(c->Dr6 & 1))
		return EXCEPTION_CONTINUE_SEARCH;
	c->Dr6 = 0;
	InterlockedIncrement(&g_tt_wp_disparos);
	DWORD eip = c->Eip;
	// filtrar nuestros propios stubs/helper (estan en este dll)
	if (g_tt_este_dll) {
		unsigned char *b = (unsigned char *)g_tt_este_dll;
		if (eip >= (DWORD)(uintptr_t)b && eip < (DWORD)(uintptr_t)b + 0x400000)
			return EXCEPTION_CONTINUE_EXECUTION;
	}
	LONG n = InterlockedCompareExchange(&g_tt_lectores_n, 0, 0);
	for (LONG i = 0; i < n; i++) {
		if (g_tt_lectores[i].eip == eip) {
			InterlockedIncrement(&g_tt_lectores[i].n);
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	if (n < TT_LECTORES_MAX) {
		LONG idx = InterlockedIncrement(&g_tt_lectores_n) - 1;
		if (idx < TT_LECTORES_MAX) {
			g_tt_lectores[idx].eip = eip;
			g_tt_lectores[idx].n = 1;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	InterlockedIncrement(&g_tt_lectores_otros);
	return EXCEPTION_CONTINUE_EXECUTION;
}

// Arma DR0 en todos los hilos del proceso (salvo el propio). Devuelve cuantos armo.
static int tt_wp_armar_todos(unsigned char *dir)
{
	int armados = 0, hilos = 0;
	DWORD pid = GetCurrentProcessId(), yo = GetCurrentThreadId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return -1;
	THREADENTRY32 te; te.dwSize = sizeof(te);
	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID != pid || te.th32ThreadID == yo)
				continue;
			hilos++;
			HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
			if (!h)
				continue;
			if (SuspendThread(h) != (DWORD)-1) {
				CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
				ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(h, &ctx)) {
					DWORD want_dr0 = (DWORD)(uintptr_t)dir;
					DWORD want_dr7 = (ctx.Dr7 & ~(0x3u | (0xFu << 16))) | 0x1u | (0x3u << 16) | (0x3u << 18);
					if (ctx.Dr0 != want_dr0 || (ctx.Dr7 & 0xF0003u) != (want_dr7 & 0xF0003u)) {
						ctx.Dr0 = want_dr0;
						ctx.Dr7 = want_dr7;
						ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (SetThreadContext(h, &ctx))
							armados++;
					}
				}
				ResumeThread(h);
			}
			CloseHandle(h);
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	InterlockedExchange(&g_tt_wp_hilos, hilos);
	return armados;
}

static DWORD WINAPI tt_wp_hilo(LPVOID)
{
	AddVectoredExceptionHandler(1, tt_veh);
	unsigned char *ultima = NULL;
	for (;;) {
		Sleep(2000);
		unsigned char *cam = g_tt_gamecam;
		if (!cam)
			continue;
		unsigned char *dir = cam + TT_OFF_GC_FOV;
		int n = tt_wp_armar_todos(dir);
		if (n > 0)
			InterlockedExchangeAdd(&g_tt_wp_armados, n);
		if (dir != ultima) {
			LogInfo("TTFOV: breakpoint de lectura armado en %p (camara del juego %p, +0x50); hilos=%ld armados ahora=%d\n",
				dir, cam, InterlockedCompareExchange(&g_tt_wp_hilos, 0, 0), n);
			g_tt_wp_dir = dir;
			ultima = dir;
		}
	}
}

static bool tt_fov_parchear(unsigned char *sitio, void *stub, unsigned char *orig)
{
	DWORD viejo = 0;
	if (!VirtualProtect(sitio, 5, PAGE_EXECUTE_READWRITE, &viejo))
		return false;
	memcpy(orig, sitio, 5);
	unsigned char parche[5]; parche[0] = 0xE8;
	LONG rel = (LONG)((uintptr_t)stub - (uintptr_t)(sitio + 5));
	memcpy(parche + 1, &rel, 4);
	memcpy(sitio, parche, 5);
	VirtualProtect(sitio, 5, viejo, &viejo);
	FlushInstructionCache(GetCurrentProcess(), sitio, 5);
	return true;
}

static bool tt_fov_init(unsigned char *exe)
{
	// exe+0x5D6B20: mov edi,[ebx+0xC]; movss xmm2,[1.0]; movss xmm1,[ebp-0x64]; comiss xmm2,xmm1;
	//               movss xmm0,[edi+0x50]; movss [ebp-0x40],xmm0; jbe +9
	static const unsigned char s_a[] = { 0x8B,0x7B,0x0C, 0xF3,0x0F,0x10,0x15,0,0,0,0, 0xF3,0x0F,0x10,0x4D,0x9C, 0x0F,0x2F,0xD1,
		0xF3,0x0F,0x10,0x47,0x50, 0xF3,0x0F,0x11,0x45,0xC0, 0x76,0x09 };
	static const unsigned char m_a[sizeof(s_a)] = { 1,1,1, 1,1,1,1,0,0,0,0, 1,1,1,1,1, 1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1 };
	// exe+0x5D6F22: mov esi,[ebx+8]; mov edx,[ebx+0xC]; movss xmm0,[edx+0x50]; mulss xmm0,[c]; mulss xmm0,[c]; movss [esi+0xE4],xmm0
	static const unsigned char s_b[] = { 0x8B,0x73,0x08, 0x8B,0x53,0x0C, 0xF3,0x0F,0x10,0x42,0x50, 0xF3,0x0F,0x59,0x05,0,0,0,0,
		0xF3,0x0F,0x59,0x05,0,0,0,0, 0xF3,0x0F,0x11,0x86,0xE4,0x00,0x00,0x00 };
	static const unsigned char m_b[sizeof(s_b)] = { 1,1,1, 1,1,1, 1,1,1,1,1, 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0, 1,1,1,1,1,1,1,1 };
	bool ok = false;
	__try {
		ok = tt_firma(exe, 0x5D6B20, s_a, m_a, sizeof(s_a)) && tt_firma(exe, 0x5D6F22, s_b, m_b, sizeof(s_b));
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTFOV: las firmas de las lecturas del fov de la camara del juego no coinciden; inerte.\n");
		InterlockedExchange(&g_tt_fov_state, -1);
		return false;
	}
	g_tt_fov_sitio_a = exe + TT_RVA_FOV_LEE_A;
	g_tt_fov_sitio_b = exe + TT_RVA_FOV_LEE_B;
	if (!tt_fov_parchear(g_tt_fov_sitio_a, (void *)&tt_fov_stub_edi, g_tt_fov_orig_a) ||
	    !tt_fov_parchear(g_tt_fov_sitio_b, (void *)&tt_fov_stub_edx, g_tt_fov_orig_b)) {
		LogInfo("TTFOV: VirtualProtect fallo (%lu); inerte.\n", GetLastError());
		InterlockedExchange(&g_tt_fov_state, -1);
		return false;
	}
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&tt_fov_init, &g_tt_este_dll);
	InterlockedExchange(&g_tt_fov_state, 1);

	// BUILD 14: el operador de copia de la camara (exe+0x0DE4D0). Firma de los 38
	// bytes exe+0x0DE514..0x0DE53A (movq +0x40/+0x48, fld/fstp +0x50/+0x54/+0x58).
	{
		static const unsigned char s_cp[] = {
			0xF3,0x0F,0x7E,0x41,0x40, 0x66,0x0F,0xD6,0x40,0x40, 0xF3,0x0F,0x7E,0x41,0x48, 0x66,0x0F,0xD6,0x40,0x48,
			0xD9,0x41,0x50, 0xD9,0x58,0x50, 0xD9,0x41,0x54, 0xD9,0x58,0x54, 0xD9,0x41,0x58, 0xD9,0x58,0x58 };
		static const unsigned char m_cp[sizeof(s_cp)] = {
			1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1 };
		bool ok_cp = false;
		__try {
			ok_cp = tt_firma(exe, 0x0DE514, s_cp, m_cp, sizeof(s_cp));
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok_cp = false;
		}
		unsigned char *sitio = exe + 0x0DE528;
		DWORD viejo = 0;
		if (ok_cp && VirtualProtect(sitio, 6, PAGE_EXECUTE_READWRITE, &viejo)) {
			memcpy(g_tt_fov_copia_orig, sitio, 6);
			unsigned char parche[6]; parche[0] = 0xE8; parche[5] = 0x90;
			LONG rel = (LONG)((uintptr_t)&tt_fov_copia_stub - (uintptr_t)(sitio + 5));
			memcpy(parche + 1, &rel, 4);
			memcpy(sitio, parche, 6);
			VirtualProtect(sitio, 6, viejo, &viejo);
			FlushInstructionCache(GetCurrentProcess(), sitio, 6);
			g_tt_fov_copia_sitio = sitio;
			InterlockedExchange(&g_tt_fov_copia_state, 1);
			LogInfo("TTFOV: operador de copia de la camara (exe+0x0DE528) redirigido al stub %p.\n", &tt_fov_copia_stub);
		} else {
			InterlockedExchange(&g_tt_fov_copia_state, -1);
			LogInfo("TTFOV: la firma del operador de copia no coincide (o VirtualProtect fallo); el experimento del fov ancho queda inerte.\n");
		}
	}
	HANDLE h = CreateThread(NULL, 0, tt_wp_hilo, NULL, 0, NULL);
	if (h) CloseHandle(h);
	LogInfo("TTFOV: lecturas del fov de la camara del juego redirigidas (exe+0x%X, exe+0x%X); fov ancho = %.5f rad (%.1f grados); vigilancia de lectores en marcha.\n",
		TT_RVA_FOV_LEE_A, TT_RVA_FOV_LEE_B, g_tt_fov_wide, g_tt_fov_wide * 57.29578f);
	return true;
}

static void tt_fov_resumen(unsigned char *exe, unsigned frame)
{
	if (InterlockedCompareExchange(&g_tt_fov_state, 0, 0) != 1)
		return;
	unsigned char *cam = g_tt_gamecam;
	if (cam && !g_tt_gamecam_volcada) {
		g_tt_gamecam_volcada = true;
		__try {
			const float *f = (const float *)(cam + 0x40);
			LogInfo("TTFOV: camara del juego %p, campos +0x40..+0xBC: %.3f %.3f %.3f %.3f | %.5f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
				cam, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15],
				f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23], f[24], f[25], f[26], f[27], f[28], f[29], f[30], f[31]);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	LogInfo("TTFOV: resumen frame=%u cam=%p fov_orig=%.5f (%.1f grados) lecturas_frame=%ld copias=%ld copias_anchas=%ld copia_estado=%ld | breakpoint dir=%p hilos=%ld armados_total=%ld disparos=%ld lectores=%ld otros=%ld\n",
		frame, cam, g_tt_fov_orig, g_tt_fov_orig * 57.29578f,
		InterlockedCompareExchange(&g_tt_fov_lecturas, 0, 0), InterlockedCompareExchange(&g_tt_fov_copias, 0, 0),
		InterlockedCompareExchange(&g_tt_fov_copias_anchas, 0, 0), InterlockedCompareExchange(&g_tt_fov_copia_state, 0, 0), g_tt_wp_dir,
		InterlockedCompareExchange(&g_tt_wp_hilos, 0, 0), InterlockedCompareExchange(&g_tt_wp_armados, 0, 0),
		InterlockedCompareExchange(&g_tt_wp_disparos, 0, 0), InterlockedCompareExchange(&g_tt_lectores_n, 0, 0),
		InterlockedCompareExchange(&g_tt_lectores_otros, 0, 0));
	LONG n = InterlockedCompareExchange(&g_tt_lectores_n, 0, 0);
	if (n > TT_LECTORES_MAX) n = TT_LECTORES_MAX;
	for (LONG i = 0; i < n; i++) {
		DWORD eip = g_tt_lectores[i].eip;
		LONG c = InterlockedCompareExchange(&g_tt_lectores[i].n, 0, 0);
		if (eip >= (DWORD)(uintptr_t)exe && eip < (DWORD)(uintptr_t)exe + 0x1000000)
			LogInfo("TTFOV:   lector exe+0x%06X  n=%ld\n", (unsigned)(eip - (DWORD)(uintptr_t)exe), c);
		else
			LogInfo("TTFOV:   lector %08X (fuera del exe)  n=%ld\n", eip, c);
	}
}

static void tt_fov_quitar()
{
	if (InterlockedCompareExchange(&g_tt_fov_state, 0, 0) != 1)
		return;
	// primero desarmar el breakpoint en todos los hilos (Dr7 = 0 para L0)
	__try {
		DWORD pid = GetCurrentProcessId(), yo = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			THREADENTRY32 te; te.dwSize = sizeof(te);
			if (Thread32First(snap, &te)) {
				do {
					if (te.th32OwnerProcessID != pid || te.th32ThreadID == yo) continue;
					HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
					if (!h) continue;
					if (SuspendThread(h) != (DWORD)-1) {
						CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
						if (GetThreadContext(h, &ctx) && (ctx.Dr7 & 1)) {
							ctx.Dr0 = 0; ctx.Dr7 &= ~(0x3u | (0xFu << 16)); ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
							SetThreadContext(h, &ctx);
						}
						ResumeThread(h);
					}
					CloseHandle(h);
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	DWORD viejo = 0;
	if (InterlockedCompareExchange(&g_tt_fov_copia_state, 0, 0) == 1 && g_tt_fov_copia_sitio &&
	    VirtualProtect(g_tt_fov_copia_sitio, 6, PAGE_EXECUTE_READWRITE, &viejo)) {
		memcpy(g_tt_fov_copia_sitio, g_tt_fov_copia_orig, 6); VirtualProtect(g_tt_fov_copia_sitio, 6, viejo, &viejo);
		FlushInstructionCache(GetCurrentProcess(), g_tt_fov_copia_sitio, 6);
		InterlockedExchange(&g_tt_fov_copia_state, -1);
	}
	if (g_tt_fov_sitio_a && VirtualProtect(g_tt_fov_sitio_a, 5, PAGE_EXECUTE_READWRITE, &viejo)) {
		memcpy(g_tt_fov_sitio_a, g_tt_fov_orig_a, 5); VirtualProtect(g_tt_fov_sitio_a, 5, viejo, &viejo);
	}
	if (g_tt_fov_sitio_b && VirtualProtect(g_tt_fov_sitio_b, 5, PAGE_EXECUTE_READWRITE, &viejo)) {
		memcpy(g_tt_fov_sitio_b, g_tt_fov_orig_b, 5); VirtualProtect(g_tt_fov_sitio_b, 5, viejo, &viejo);
	}
	InterlockedExchange(&g_tt_fov_state, -1);
	LogInfo("TTFOV: stubs y breakpoint retirados.\n");
}

static void tt_cam_hook_quitar()
{
	tt_fov_quitar();
	if (InterlockedCompareExchange(&g_tt_inl_state, 0, 0) == 1 && g_tt_inl_sitio) {
		DWORD viejo = 0;
		if (VirtualProtect(g_tt_inl_sitio, 5, PAGE_EXECUTE_READWRITE, &viejo)) {
			memcpy(g_tt_inl_sitio, g_tt_inl_orig, 5);
			VirtualProtect(g_tt_inl_sitio, 5, viejo, &viejo);
			FlushInstructionCache(GetCurrentProcess(), g_tt_inl_sitio, 5);
		}
		InterlockedExchange(&g_tt_inl_state, -1);
		LogInfo("TTINL: divss restaurado.\n");
	}
	if (InterlockedCompareExchange(&g_tt_eye_state, 0, 0) == 1 && g_tt_eye_call) {
		tt_persp_escribir_rel32(g_tt_eye_call, (void *)g_tt_eye_orig);
		InterlockedExchange(&g_tt_eye_state, -1);
		LogInfo("TTEYE: call restaurado.\n");
	}
	if (InterlockedCompareExchange(&g_tt_ovr_state, 0, 0) == 1 && g_tt_ovr_call) {
		tt_persp_escribir_rel32(g_tt_ovr_call, (void *)g_tt_ovr_orig);
		InterlockedExchange(&g_tt_ovr_state, -1);
		LogInfo("TTOVR: call restaurado.\n");
	}
	if (InterlockedCompareExchange(&g_tt_persp_state, 0, 0) == 1 && g_tt_persp_call) {
		tt_persp_escribir_rel32(g_tt_persp_call, (void *)g_tt_persp_orig);
		InterlockedExchange(&g_tt_persp_state, -1);
		LogInfo("TTPERSP: call restaurado.\n");
	}
	if (InterlockedCompareExchange(&g_tt_cam_state, 0, 0) != 1 || !g_tt_cam_slot)
		return;
	DWORD viejo = 0;
	if (VirtualProtect(g_tt_cam_slot, sizeof(DWORD), PAGE_READWRITE, &viejo)) {
		InterlockedExchange((volatile LONG *)g_tt_cam_slot, (LONG)(uintptr_t)g_tt_cam_build_orig);
		VirtualProtect(g_tt_cam_slot, sizeof(DWORD), viejo, &viejo);
	}
	InterlockedExchange(&g_tt_cam_state, -1);
	LogInfo("TTCAM: hook retirado (vtable restaurada).\n");
}

struct tt_cam_guardia { ~tt_cam_guardia() { tt_cam_hook_quitar(); } };
static tt_cam_guardia g_tt_cam_guardia;

static bool tt_cam_init()
{
	LONG estado = InterlockedCompareExchange(&g_tt_cam_state, 2, 0);
	if (estado == 1)
		return true;
	if (estado != 0)
		return false;

	unsigned char *exe = (unsigned char *)GetModuleHandleA(NULL);
	if (!exe) {
		LogInfo("TTCAM: no pude obtener la base del exe; hook inerte.\n");
		InterlockedExchange(&g_tt_cam_state, -1);
		return false;
	}

	// 1. prologo de "construir la proyeccion" (exe+0x4EC6F0), 61 bytes, sin
	//    operandos absolutos: push ebx; mov ebx,esp; ...; mov esi,ecx;
	//    cmp byte [esi+0xE0],0; push edi; je ...; movss xmm0,[esi+0xE8]; movss xmm1,[esi+0xE4]
	static const unsigned char s_build[] = {
		0x53,0x8B,0xDC,0x83,0xEC,0x08,0x83,0xE4,0xF0,0x83,0xC4,0x04,0x55,0x8B,0x6B,0x04,
		0x89,0x6C,0x24,0x04,0x8B,0xEC,0x81,0xEC,0xC8,0x00,0x00,0x00,0x56,0x8B,0xF1,0x80,
		0xBE,0xE0,0x00,0x00,0x00,0x00,0x57,0x0F,0x84,0xE0,0x00,0x00,0x00,0xF3,0x0F,0x10,
		0x86,0xE8,0x00,0x00,0x00,0xF3,0x0F,0x10,0x8E,0xE4,0x00,0x00,0x00 };
	static const unsigned char m_build[sizeof(s_build)] = {
		1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
		1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1 };
	// 2. el constructor escribiendo los campos (exe+0x52B97D): movss [esi+0xE4],xmm0;
	//    movss xmm0,[far]; movss [esi+0xE8],xmm0; movss xmm0,[near]; movss [esi+0xEC],xmm0;
	//    movss xmm0,[1.0]; movss [esi+0xF0],xmm0; ... ; mov [esi],vtable; mov byte [esi+0xE0],1
	static const unsigned char s_ctor[] = {
		0xF3,0x0F,0x11,0x86,0xE4,0x00,0x00,0x00, 0xF3,0x0F,0x10,0x05,0,0,0,0,
		0xF3,0x0F,0x11,0x86,0xE8,0x00,0x00,0x00, 0xF3,0x0F,0x10,0x05,0,0,0,0,
		0xF3,0x0F,0x11,0x86,0xEC,0x00,0x00,0x00, 0xF3,0x0F,0x10,0x05,0,0,0,0,
		0xF3,0x0F,0x11,0x86,0xF0,0x00,0x00,0x00, 0xF3,0x0F,0x11,0x86,0xF4,0x00,0x00,0x00,
		0xF3,0x0F,0x11,0x8E,0xF8,0x00,0x00,0x00, 0xF3,0x0F,0x11,0x8E,0xFC,0x00,0x00,0x00,
		0x0F,0x28,0x0D,0,0,0,0, 0xF3,0x0F,0x11,0x86,0x00,0x01,0x00,0x00,
		0x0F,0x28,0x05,0,0,0,0, 0xC7,0x06,0,0,0,0, 0xC6,0x86,0xE0,0x00,0x00,0x00,0x01 };
	static const unsigned char m_ctor[sizeof(s_ctor)] = {
		1,1,1,1,1,1,1,1, 1,1,1,1,0,0,0,0,
		1,1,1,1,1,1,1,1, 1,1,1,1,0,0,0,0,
		1,1,1,1,1,1,1,1, 1,1,1,1,0,0,0,0,
		1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
		1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
		1,1,1,0,0,0,0, 1,1,1,1,1,1,1,1,
		1,1,1,0,0,0,0, 1,1,0,0,0,0, 1,1,1,1,1,1,1 };

	bool ok = false;
	DWORD *slot = (DWORD *)(exe + TT_RVA_CAM_VTABLE + TT_CAM_SLOT_BUILD * 4);
	__try {
		ok = tt_firma(exe, TT_RVA_CAM_BUILD, s_build, m_build, sizeof(s_build))
		  && tt_firma(exe, TT_RVA_CAM_CTOR_E4, s_ctor, m_ctor, sizeof(s_ctor))
		  && tt_dword_es(exe, TT_RVA_CAM_CTOR_E4 + 0x68, exe + TT_RVA_CAM_VTABLE)
		  && slot[0] == (DWORD)(uintptr_t)(exe + TT_RVA_CAM_BUILD)
		  && slot[1] == (DWORD)(uintptr_t)(exe + TT_RVA_CAM_SUBRECT)
		  && slot[2] == (DWORD)(uintptr_t)(exe + TT_RVA_CAM_SLOT18);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}
	if (!ok) {
		LogInfo("TTCAM: las firmas de la camara de drt.exe 1.1.114.5105 no coinciden; hook inerte (fail-open).\n");
		InterlockedExchange(&g_tt_cam_state, -1);
		return false;
	}

	InitializeCriticalSection(&g_tt_cam_cs);
	g_tt_cam_exe = exe;
	g_tt_cam_slot = slot;
	g_tt_cam_build_orig = (tt_cam_build_fn)(uintptr_t)slot[0];
#if TT_CULL_DIAG
	// Solo en la build de diagnostico: el hook de la vtable (anota), los dos
	// constructores que nunca corren en esta maquina (TTPERSP, TTOVR) y la
	// vigilancia del fov de la camara del juego (stubs + breakpoint de hardware +
	// operador de copia). Ver docs/INVESTIGACION-CULLING.md, builds 1 a 14.
	DWORD viejo = 0;
	if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &viejo)) {
		LogInfo("TTCAM: VirtualProtect fallo (%lu); hook inerte.\n", GetLastError());
		InterlockedExchange(&g_tt_cam_state, -1);
		return false;
	}
	InterlockedExchange((volatile LONG *)slot, (LONG)(uintptr_t)&tt_cam_build_hook);
	VirtualProtect(slot, sizeof(DWORD), viejo, &viejo);
	InterlockedExchange(&g_tt_cam_state, 1);
	LogInfo("TTCAM: hook instalado en la vtable de la camara (exe+0x%X slot %d: exe+0x%X -> %p). "
		"Solo anota; el ensanche va por TTEYE/TTINL.\n",
		TT_RVA_CAM_VTABLE, TT_CAM_SLOT_BUILD, TT_RVA_CAM_BUILD, &tt_cam_build_hook);
	tt_persp_init(exe);
	tt_ovr_init(exe);
	tt_inl_init(exe);
	tt_fov_init(exe);
#else
	// Producto (desde la build 15, 2026-09-04): la vtable NO se toca. Lo unico que
	// corre es el ensanche de la matriz de proyeccion de la escena (TTEYE + TTINL),
	// que es lo que el autor vio andar: arbustos, objetos y terreno lejano en los
	// laterales, sin costo de fps. Las firmas de la camara se verificaron arriba
	// igual: si el exe no es el esperado, no se instala nada.
	(void)&tt_cam_build_hook;
	(void)&tt_persp_init;
	(void)&tt_ovr_init;
	(void)&tt_fov_init;
	InterlockedExchange(&g_tt_cam_state, 1);
	tt_inl_init(exe);
	LogInfo("TTCAM: firmas de la camara OK; ensanche de la matriz de escena por TTEYE/TTINL (vtable sin tocar).\n");
#endif
	return true;
}

static void tt_cam_update()
{
	if (InterlockedCompareExchange(&g_tt_cam_state, 0, 0) == 0)
		tt_cam_init();
	if (InterlockedCompareExchange(&g_tt_cam_state, 0, 0) != 1)
		return;
	// $cull_widen la cicla la tecla B (0, 8, 1): cualquier valor distinto de 0 enciende.
	if (!g_tt_cam_ini_widen)
		g_tt_cam_ini_widen = cl_global(L"$cull_widen");
	// SOLO MANEJANDO (build 16, 2026-09-04). Con el ensanche encendido por defecto,
	// el MENU salio aplastado a lo ancho: sus camaras 3D tienen el mismo aspecto
	// de triple que la de pista y pasan por el mismo constructor. El dato que
	// separa pista de menu ya lo lee el detector de repeticion: el estado del
	// neRacing::ReplaySystem, 7 = grabando = manejando (0 en menus y cargas,
	// 1.. en repeticiones). Si el detector esta inerte (-9999), no se ensancha:
	// el error cae del lado de no tocar nada.
	// BUILD 17: rs == 7 NO discrimina (el menu principal tambien esta en 7). Lo que
	// discrimina es el CONTENIDO: los vertex shaders del terreno solo se dibujan en
	// un tramo (HackerContext::VSSetShader avisa por TripleNotaVS). "Manejando" =
	// terreno visto en los ultimos 2 s y el detector de repeticion en JUEGO (en la
	// repeticion hay terreno pero el mod esta apagado y la proyeccion ancha se
	// veria aplastada). Menu, garage, cargas y resultados: sin terreno -> nada.
	unsigned frame_now = G ? G->frame_no : 0;
	LONG ult = InterlockedCompareExchange(&g_tt_terreno_frame, 0, 0);
	bool terreno_reciente = ult != 0 && (frame_now - (unsigned)ult) <= 120;
	// BUILD 18: y ADEMAS solo con el triple realmente ACTIVO. Con el mod apagado
	// (F7 -> $triple_on = 0) la matriz ancha se veia aplastada, porque nadie la
	// recalibraba. y3 es lo que [Present] publica como "triple activo"
	// ($triple_on == 1 && $es_conduccion == 1): es la MISMA senal que decide si se
	// bindea la textura de region o la passthrough, asi que el ensanche no puede
	// quedar prendido con el triple apagado, por construccion.
	bool triple_activo = G && G->iniParams.size() > 3 && G->iniParams[3].y >= 0.5f;
	LONG manejando = (terreno_reciente && triple_activo && !tt_replay_activo()) ? 1 : 0;
	LONG widen = (g_tt_cam_ini_widen && g_tt_cam_ini_widen->fval != 0.0f && manejando) ? 1 : 0;
	LONG val = g_tt_cam_ini_widen ? (LONG)(g_tt_cam_ini_widen->fval + 0.5f) : 0;
	LONG antes_val = InterlockedExchange(&g_tt_cam_widen_val, val);
	LONG antes = InterlockedExchange(&g_tt_cam_widen, widen);
	if (antes != widen || antes_val != val)
		LogInfo("TTCAM: ensanche %s ($cull_widen=%ld, rs=%d: solo se aplica manejando; objetivo %.0f grados horizontales)\n",
			widen ? "ENCENDIDO" : "APAGADO", val, g_tt_probe_rs, TT_CULL_HFOV);
	// Cada 600 frames, un resumen: formas distintas, desbordes y APLICACIONES.
	static unsigned ultimo = 0;
	unsigned frame = G ? G->frame_no : 0;
	if (frame - ultimo >= 600) {
		ultimo = frame;
		EnterCriticalSection(&g_tt_cam_cs);
		LogInfo("TTCAM: resumen frame=%u widen=%ld formas=%d otras(desbordadas)=%u llamadas=%u ensanchadas=%u restauradas=%u aspect_orig=%.4f | "
			"TTPERSP estado=%ld formas=%d llamadas=%u ensanchadas=%u | TTOVR estado=%ld formas=%d llamadas=%u ensanchadas=%u descriptor=%u | "
			"TTINL estado=%ld llamadas=%ld ensanchadas=%ld | TTEYE estado=%ld llamadas=%u ensanchadas=%u\n",
			frame, widen, g_tt_cams_n, g_tt_cams_otras, g_tt_cam_llamadas, g_tt_cam_ensanchadas, g_tt_cam_restauradas, g_tt_cam_aspect_orig,
			InterlockedCompareExchange(&g_tt_persp_state, 0, 0), g_tt_persp_n, g_tt_persp_llamadas, g_tt_persp_ensanchadas,
			InterlockedCompareExchange(&g_tt_ovr_state, 0, 0), g_tt_ovr_n, g_tt_ovr_llamadas, g_tt_ovr_ensanchadas, g_tt_ovr_desc,
			InterlockedCompareExchange(&g_tt_inl_state, 0, 0), InterlockedCompareExchange(&g_tt_inl_llamadas, 0, 0), InterlockedCompareExchange(&g_tt_inl_ensanchadas, 0, 0),
			InterlockedCompareExchange(&g_tt_eye_state, 0, 0), g_tt_eye_llamadas, g_tt_eye_ensanchadas);
		LeaveCriticalSection(&g_tt_cam_cs);
		tt_fov_resumen(g_tt_cam_exe, frame);
	}
}

// ⚠️ `data` es EL MAPEO VIVO del cbuffer, no una copia: `HackerContext::Unmap`
// llama aca ANTES del Unmap real. Por eso puede escribirse, y por eso se puede
// deshacer el ensanche del frustum justo antes de que el buffer llegue a la GPU
// (ver tt_restaurar_m00). Era `const void *` mientras esto solo leia.
void TripleCaptureCameraCB(void *data, unsigned size)
{
	// El estado de repeticion se actualiza ANTES de filtrar el buffer: es del
	// motor, no de una camara. [Present] lo recibe en $en_replay.
	tt_replay_update();
	tt_car_update();
	tt_cam_update();

	// EL ENSANCHE SE QUEDA DEL LADO DE LA CPU. El motor ya hizo su descarte con
	// la matriz ancha; lo que va a los shaders vuelve a ser la original, asi que
	// NINGUN shader -ni los que el mod no parchea: polvo, humo, particulas,
	// cielo- puede salir aplastado. Va antes del gate de abajo a proposito: el
	// gate filtra por aspecto, y una camara ensanchada tiene otro aspecto.
	if (data && size >= 208)
		tt_restaurar_m00((float *)data, size / 4);

	// size >= 208 -> hacen falta al menos 52 floats para leer f[51]
	// (relativeViewProjection col3.row3).
	//
	// MEDIDO (sesion 01, docs\SESION-01-CAMARAS.md 2.2): en DR1 el cbuffer real
	// mide 272 BYTES = 17 registros, no 320. Este comentario decia 320, que es el
	// total DECLARADO en el pack: incluye un `padding3` de 36 bytes (cb3[17..19])
	// que en el buffer real NO EXISTE. No hacia dano -la guarda es `size >= 208`,
	// y 272 la pasa con margen- pero era un dato falso en el fuente.
	// (Hallazgo 12c de la revision adversarial de la sesion 01.)
	if (!data || size < 208 || size > sizeof(g_triple_cam_buf))
		return;
	const float *f = (const float*)data;
	float aspect = (f[0] != 0.0f) ? (f[5] / f[0]) : 0.0f;
	// Gate: solo la camara PRINCIPAL (perspectiva m33~0 + aspecto de triple).
	// El rango cubre de 3x 5:4 (3.75) a 3x 32:9 (10.67) con margen. El piso 3.6
	// excluye los espejos y el envmap.
	//
	// CERRADO por la sesion 01 (docs\SESION-01-CAMARAS.md 4.2). El TODO decia
	// "en DR1 el tamano no esta medido"; ya esta medido, y con dos capas:
	//   espejos: aspecto 3.00000  (98 draws)   -> NO pasa el > 3.6
	//   envmap : aspecto 1.00000  (407 draws)  -> NO pasa el > 3.6
	// Y -esto es lo que lo hace valido pese a que las capturas salieron a
	// 1024x768- esos dos numeros NO salen de la pantalla, salen de la config del
	// usuario: <mirrors width="768" height="256"> da 768/256 = 3.000 EXACTO y
	// <envmap size="1024"> es cuadrado y da 1.000. Cambiar la resolucion de
	// pantalla no los mueve. La orto chica del coche (1.87) y las cascadas CSM
	// (1.00) ya las corta la guarda de f[51].
	//
	// RIESGO RESIDUAL, y no es teorico: los dos numeros dependen de <mirrors>. Si
	// el usuario sube el espejo a, por ejemplo, 3072x256, el aspecto pasa a 12.0 y
	// ENTRA en el filtro -> el mod capturaria la camara del espejo como si fuera
	// la principal. El limite superior de 12.0 no es gratis. La app deberia LEER
	// <mirrors> de hardware_settings_config.xml y avisar si width/height cae
	// dentro de (3.6, 12.0).
	// (Hallazgo 12b de la revision adversarial de la sesion 01.)
	//
	// 2026-09-04, rama culling: el ensanche de tt_cam_build_hook escribe en la
	// camara del motor un aspecto de hasta ~70 (fov 23 -> 14.3/tan(11.5)). El
	// limite superior sube a 80 para que la camara ensanchada SIGA entrando; el
	// riesgo del espejo de 12.0 queda igual de teorico (nadie configura un espejo
	// de 80:1).
	// Build 6: con 178 grados el aspecto ensanchado llega a ~280 (fov 23). El tope
	// deja de ser un numero "razonable": queda solo como cota contra basura.
	if (f[51] < 0.5f && aspect > 3.6f && aspect < 100000.0f) {
		memcpy(g_triple_cam_buf, data, size);
		g_triple_cam_size = size;
		g_triple_cam_valid = true;
		// DETECCION DE MENU. La camara de fondo del frontend tiene un m11 muy
		// alto comparado con las de conduccion. Si en los ultimos frames se
		// escribio una camara con m11 > 3.0, publicamos x3 = 1 y el d3dx.ini
		// pasa el triple a passthrough -> los menus se ven como el juego normal.
		// TODO(descubrimiento) DR1: el umbral 3.0 sale de DR2 (menu 4.27/3.55,
		// conduccion 1.9..2.5, garage 1.0). En DR1 hay que MEDIRLO con las
		// lineas "TRIPLE camera #N" del log antes de confiar en el.
		{
			static unsigned g_triple_menu_frame = 0x80000000;
			if (f[5] > 3.0f)
				g_triple_menu_frame = G->frame_no;
			if (G->iniParams.size() >= 4)
				G->iniParams[3].x = (G->frame_no - g_triple_menu_frame <= 3) ? 1.0f : 0.0f;
		}

		// ⚠️ ACA SE PUBLICABA m11 EN IniParams[3].w, DESDE CUALQUIER CAMARA QUE
		// PASARA EL GATE — o sea, la ULTIMA del frame. Se movio al bloque de
		// abajo, junto al near, por el motivo que ese bloque ya explica largo:
		// en un frame pasan siete camaras distintas (garage, espejos, reflejos,
		// cockpit...) y gana la que venga ultima, que casi nunca es aquella a
		// traves de la cual se esta mirando.
		//
		// Mientras m11 lo leia solo el ini para calcular la distancia de ojo
		// "exacta" eso era inofensivo. Deja de serlo ahora que m11 y el near
		// deciden JUNTOS si la camara es de conduccion: si cada uno viene de una
		// camara distinta, la decision se toma sobre una camara que no existe.
		//
		// Se comprobo que nadie mas lo lee: `w3` esta documentado en la
		// plantilla pero no aparece en ninguna condicion ni en ningun patron.

		// El CABECEO de la camara respecto del horizonte del mundo, que es toda
		// la señal del estabilizador de la mirada del d3dx.ini.
		//
		// relativeView (cb3[13..15]) es rotacion pura y row_major, o sea floats
		// 52..63: la fila 2 es el vector ADELANTE de la camara en coordenadas de
		// mundo, y su componente Y —f[61]— es el SENO del cabeceo. Se elige esa y
		// no la del vector ARRIBA porque esa ultima va como el coseno: cerca de 1
		// y casi sin moverse con angulos chicos (medido en DR2: se mueve diez
		// veces menos). Va entre -1 y 1, asi que no tiene el problema de
		// precision que tuvo la altura absoluta.
		//
		// TODO(descubrimiento) DR1: el layout del cbuffer es identico al de DR2
		// (verificado, 3031/3031 usos), pero que relativeView sea rotacion pura y
		// row_major TAMBIEN aca es deduccion, no medicion. La primera corrida lo
		// contesta sola: el valor se ve en el cartel y tiene que moverse al
		// inclinar la vista y casi nada al subir el asiento.
		//
		// DENTRO del gate a proposito. En DR2, escribir esto fuera de la
		// condicion de camara arruino w3 y la deteccion de vista por m11: el
		// frontend tambien pasa por aca y pisaria el valor con su propia camara.
		// La condicion es la misma que usa el resto de este bloque: que no sea
		// camara de menu (m11 <= 3.0) y que sea perspectiva real (m22 < -0.9,
		// para descartar una ortografica que se cuele).
		//
		// Y el `size >= 248` NO es de adorno: la guarda de arriba de esta funcion
		// solo exige 208 bytes (52 floats, hasta f[51]), y f[61] necesita 248. El
		// cbuffer medido en DR1 son 272, asi que en la practica entra — pero
		// apoyarse en eso seria leer fuera del buffer el dia que aparezca una
		// camara con un cbuffer mas corto. DR2 tiene exactamente esta lectura SIN
		// esta condicion; esta latente alla y no se replica aca.
		// UNA SOLA CAMARA POR FRAME, y no "la ultima que pase".
		//
		// La primera version escribia desde cualquier camara que pasara el gate,
		// y eso NO funciona: en un frame pasan siete distintas (garage, espejos,
		// reflejos, cockpit...), todas escriben esta misma fila, y gana la que
		// venga ultima. Como la mayoria mira horizontal, su cabeceo es 0 -> el
		// estabilizador recibia 0 SIEMPRE y no se movia. Medido en el log del
		// autor: la del cockpit traia 0.026620 y la de al lado, con near=10.005,
		// traia 0.000000.
		//
		// Es la leccion que DR2 ya habia dejado escrita para el gate del agua
		// ("NO puede ser la ultima camara del frame") y que aca no se aplico.
		//
		// El criterio es el PLANO CERCANO mas chico: la camara a traves de la
		// cual se esta mirando es, por construccion, la que tiene el near mas
		// corto (cockpit 0.08, exteriores 0.1-0.4). Las de reflejos y sombras
		// estan mucho mas lejos — la que ensuciaba tenia near 10. No hace falta
		// elegir ningun umbral: se comparan entre ellas dentro del mismo frame.
		if (size >= 248 && f[5] <= 3.0f && f[10] < -0.9f && G->iniParams.size() >= 10) {
			static unsigned s_frame_cabeceo = 0xFFFFFFFFu;
			static float s_near_cabeceo = 0.0f;
			// m23 viene negativo; se compara en valor absoluto.
			float cerca = (f[14] < 0.0f) ? -f[14] : f[14];
			if (G->frame_no != s_frame_cabeceo) {
				s_frame_cabeceo = G->frame_no;
				s_near_cabeceo = 3.4e38f;
			}

			// ⚠️ LOS PASES OFFSCREEN DEL MOTOR SE SALTEAN, Y ESTO NO ES UN
			// DETALLE: sin esto, "el near mas chico" elige uno de ELLOS y toda la
			// decision de abajo se toma sobre una camara que el usuario no esta
			// mirando.
			//
			// Son perspectivas de verdad —pasan el gate de arriba— y usan un near
			// CONSTANTE de 0.2:
			//     envmap (cara de cubo 90 grados)     near 0.2   m11 1.00000
			//     perspectiva de frontend / UI 3D     near 0.2   m11 2.64782
			// Medido: aparecen en LAS 19 capturas de frame, siempre con esos
			// mismos valores. Son constantes del motor, no algo que varie.
			//
			// El sintoma que lo delato: en las camaras FIJAS de la repeticion
			// (near 0.39978) el mod quedaba ENCENDIDO. Con 0.2 < 0.39978, el pase
			// de frontend ganaba y publicaba su firma (0.2, 2.64782), que pasa el
			// filtro de conduccion. Lo mismo le pasaba —sin que se hubiera
			// notado— al parachoques (near 0.22) y a la persecucion (0.39999).
			// Las vistas que si andaban eran las de near MENOR que 0.2: cockpit y
			// parabrisas (0.08) y capo (0.10).
			//
			// Se excluyen por el NEAR y no por el m11 a proposito: el m11 del
			// frontend (2.64782) esta a menos de 0.4% del de la persecucion
			// (2.65694), demasiado cerca para separarlos con confianza. El near
			// 0.2 en cambio no lo tiene ninguna camara de vista (la mas cercana
			// es el parachoques con 0.22, un 10% arriba).
			//
			// Es una lista NEGRA de constantes del motor, que es la forma que DR2
			// termino adoptando (su regla dura 9) despues de que una lista blanca
			// sobre valores que el usuario puede mover se rompiera dos veces.
			const float NEAR_OFFSCREEN = 0.2f;
			bool es_offscreen = cerca > NEAR_OFFSCREEN - 0.005f && cerca < NEAR_OFFSCREEN + 0.005f;

			if (!es_offscreen && cerca < s_near_cabeceo) {
				s_near_cabeceo = cerca;
				G->iniParams[9].w = f[61];

				// v6: y el ROLIDO, por la misma via. f[53] es la componente Y
				// del vector DERECHA de relativeView (fila 0, row_major en
				// 52..55): cuanto se sale de la horizontal el eje lateral de
				// la camara, o sea el seno del rolido. Mismo criterio que el
				// cabeceo: el seno y no el coseno, porque cerca de 0 grados es
				// el que se mueve. El signo del servo ($rl_prop) se midio
				// en DR2; aca es herencia — si en pista el mundo rola MAS en
				// vez de menos, se da vuelta esa constante del ini, no esto.
				G->iniParams[9].y = f[53];

				// v6: y la ALTURA de la camara en el mundo, para el resorte
				// vertical (el "cuello"). eyePositionWS es cb3[8] = floats
				// 32..35; f[33] es su componente Y. El ini solo resta alturas
				// entre frames vecinos (centimetros), asi que el float32
				// alcanza aunque el valor absoluto sea de miles de metros.
				G->iniParams[9].z = f[33];

				// LA FIRMA DE LA CAMARA POR LA QUE SE ESTA MIRANDO: su plano
				// cercano y su m11, publicados JUNTOS y de la MISMA camara.
				//
				// Con esos dos el ini decide si es una vista de CONDUCCION
				// (interior, capo, parachoques) o no, y apaga el mod cuando no
				// lo es. Medido sobre 19 capturas de frame del autor:
				//
				//     vista          near    m11        el mod
				//     cockpit        0.080   2.194      ON
				//     parabrisas     0.080   2.864      ON
				//     capo           0.100   2.028      ON
				//     paragolpes     0.220   2.028      ON
				//     persecucion    0.400   2.657      off
				//     repeticion     0.098   1.441      off   (a bordo, x6)
				//     repeticion     0.400   1.43..5.67 off   (fijas, dron, x7)
				//
				// Hacen falta LOS DOS. El near solo no separa la camara a bordo
				// de la repeticion (0.098) del capo (0.100); el m11 solo no
				// separa una fija de repeticion (2.097) del capo (2.028).
				// Juntos, los margenes son 82% y 41%.
				//
				// Y NO se puede separar la persecucion de conduccion de la de
				// repeticion: son la MISMA camara (m11 2.65694 contra 2.65702,
				// near 0.39999 contra 0.39978). Por eso la persecucion se apaga
				// tambien manejando — decision del autor, tomada sabiendolo.
				if (G->iniParams.size() >= 8) {
					G->iniParams[7].w = cerca;   // near de ESA camara
					G->iniParams[3].w = f[5];    // y su m11, de ESA misma
				}
			}
		}

		// Calibracion: loguear cada camara DISTINTA (por m11/near) para poder
		// leer el FOV real de la camara de conduccion de DR1 y fijar la lista
		// blanca cam_m11_min/max, que son mediciones en vivo.
		// TrueTriple DR1: TABLA de firmas distintas, no "cambio respecto de la
		// anterior". La version vieja comparaba contra la ULTIMA camara vista y
		// cortaba a las 16 lineas; como DR1 alterna dos camaras frame a frame
		// (m11=1.000000 y m11=2.414214), quemaba las 16 en el arranque y la
		// camara de CONDUCCION no se registraba NUNCA - que es justo el dato que
		// hace falta para fijar cam_m11_min/max. Ahora cada firma se loguea una
		// sola vez y las que alternan no gastan cupo.
		//
		// LA TOLERANCIA ES GRUESA A PROPOSITO, y es contraintuitivo: parece que
		// una tolerancia FINA registraria "mas" camaras y por lo tanto seria mas
		// informativa. Es al reves, y en DR2 costo un diagnostico entero.
		//
		// La tabla no deduplica por IGUALDAD sino por CUBETA. El juego tiene
		// camaras con FOV ANIMADO (el zoom de entrada al garage, el FOV por
		// velocidad de la camara de persecucion) que derivan en pasos de ~0.002
		// de m11. Con tolerancia 0.001 cada paso de esa rampa entra como una
		// firma NUEVA: UNA sola camara se come las 48 entradas antes de salir
		// del garage, y las camaras de PISTA -que son justo el dato por el que
		// existe esta tabla- no se registran nunca. En DR2 paso DOS veces (con
		// tope 16 y con tope 64) y produjo una conclusion equivocada dada al
		// usuario ("cockpit DENTRO", leyendo una camara que no era esa).
		//
		// Con cubeta 0.02 esa rampa de 2.29 -> 2.42 colapsa en ~7 entradas en
		// vez de 62. El riesgo opuesto (fundir dos camaras distintas en una) es
		// chico y esta medido: las camaras reales de DR1 estan separadas por
		// decimas (1.45 .. 3.3 en 48 firmas a 7760x1440), muy por encima de
		// 0.02. Mismos numeros que quedaron en DR2 (m11 0.02, near 0.05).
		{
			const int MAX_FIRMAS = 48;
			const float TOL_M11 = 0.02f;   // ver el bloque de arriba: cubeta, no igualdad
			const float TOL_M23 = 0.05f;
			static float firma_m11[MAX_FIRMAS];
			static float firma_m23[MAX_FIRMAS];
			static int n_firmas = 0;
			bool ya_vista = false;
			for (int i = 0; i < n_firmas; i++) {
				float dm11 = f[5] - firma_m11[i]; if (dm11 < 0) dm11 = -dm11;
				float dm23 = f[14] - firma_m23[i]; if (dm23 < 0) dm23 = -dm23;
				if (dm11 <= TOL_M11 && dm23 <= TOL_M23) { ya_vista = true; break; }
			}
			if (!ya_vista && n_firmas < MAX_FIRMAS) {
				firma_m11[n_firmas] = f[5];
				firma_m23[n_firmas] = f[14];
				// `size`, `f[61]` y el tamano de IniParams se imprimen para poder
				// diagnosticar el estabilizador de la mirada SIN gastar otra
				// corrida: si el cartel muestra la correccion clavada en 0, la
				// causa es una de esas tres y esta linea dice cual.
				//   size < 248        -> el cbuffer no llega a relativeView
				//   ph(f61) = 0.000   -> llega, pero esa fila NO es el cabeceo
				//   ini < 10          -> la plantilla no nombra la fila 9
				LogInfo("TRIPLE camera #%d: m00=%.6f m11=%.6f aspect=%.3f m22=%.6f m23(near)=%.6f fovV=%.3f size=%u ph(f61)=%.6f ini=%zu\n",
					n_firmas, f[0], f[5], aspect, f[10], f[14],
					(f[5] > 0.0001f) ? 2.0f * atanf(1.0f / f[5]) * 57.29578f : 0.0f,
					size, (size >= 248) ? f[61] : 0.0f, G->iniParams.size());
				n_firmas++;
			}
		}

		// Aviso del ojo virtual: una sola vez por sesion, en cuanto hay camara.
		if (InterlockedCompareExchange(&g_hud_state, 1, 0) == 0) {
			HANDLE h = CreateThread(NULL, 0, eye_hud_thread, NULL, 0, NULL);
			if (h)
				CloseHandle(h);
		}
	}
}

// R = A * B, las tres column-major 4x4 (m[col*4+row]).
static void triple_mat_mul(float *R, const float *A, const float *B)
{
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += A[k * 4 + row] * B[col * 4 + k];
			R[col * 4 + row] = s;
		}
	}
}

// Inversa general 4x4 (column-major m[col*4+row], igual convencion que el juego:
// cada registro del cbuffer es una COLUMNA, como se ve en el ASM
// "clip = cb3[9]*x + cb3[10]*y + cb3[11]*z + cb3[12]").
// Devuelve false si es singular.
static bool triple_mat_inverse(float *out, const float *m)
{
	float inv[16];

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
			 + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
			 - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
			 + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
			 - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
			 - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
			 + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
			 - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
			 + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
			 + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
			 - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
			 + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
			 - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
			 - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
			 + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
			 - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
			 + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

	float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if (det > -1e-20f && det < 1e-20f)
		return false;
	det = 1.0f / det;
	for (int i = 0; i < 16; i++)
		out[i] = inv[i] * det;
	return true;
}

// Fracciones de cada region dentro del ancho total. Se usan para calcular el
// scissor a partir del ancho REAL del render target, de modo que funcione
// tambien en pases a resolucion reducida.
// [Present] las publica en las filas 6/7 de IniParams a partir de
// $mon_px/$bezel_px (z6 = fin izq, w6 = inicio centro, x7 = fin centro,
// y7 = inicio der). Si aun no estan publicadas (arranque temprano) se usa la
// tabla de respaldo de 3x2560 con 40 px de bisel (7760 de ancho), que es el
// escritorio Surround real del puesto (ver docs/CONTRATO-AGENTES.md).
static const float TRIPLE_FRAC[3][2] = {
	{ 0.0000000f, 0.3298969f },   // IZQ    0 .. 2560
	{ 0.3350515f, 0.6649485f },   // CENTRO 2600 .. 5160
	{ 0.6701031f, 1.0000000f },   // DER    5200 .. 7760
};

static void triple_get_frac(int region, float *lo, float *hi)
{
	if (G->iniParams.size() >= 8 && G->iniParams[6].z > 0.0f) {
		float fl  = G->iniParams[6].z;   // fin de la region izquierda
		float fcl = G->iniParams[6].w;   // inicio del centro
		float fcr = G->iniParams[7].x;   // fin del centro
		float frl = G->iniParams[7].y;   // inicio de la derecha
		switch (region) {
			case 0: *lo = 0.0f; *hi = fl;   return;
			case 1: *lo = fcl;  *hi = fcr;  return;
			default: *lo = frl; *hi = 1.0f; return;
		}
	}
	*lo = TRIPLE_FRAC[region][0];
	*hi = TRIPLE_FRAC[region][1];
}

// Consulta ROBUSTA del tamano del render target.
// ProcessParamRTSize (la que alimenta la variable rt_width del ini) solo acepta
// vistas D3D11_RTV_DIMENSION_TEXTURE2D / TEXTURE2DMS y sale dejando rt_width en
// -1 para cualquier otra (p.ej. vistas de ARRAY). Aca miramos el RECURSO, no la
// vista, asi que sirve para arrays y cualquier dimension de vista.
//
// RIESGO CONOCIDO (regla dura 3): OMGetRenderTargets es un metodo Get*, y la
// regla dura 3 prohibe los Get* de D3D11 en contextos DIFERIDOS porque devuelven
// basura. Se usa igual, a proposito, y esta es la justificacion completa -- va
// aca y no solo en el .ini.template, porque el que lee este codigo tiene que
// poder decidir sin abrir otro archivo:
//
//   1. MEDIDO (sesion 01, docs\SESION-01-LOG.md 3): DR1 llama UNA vez a
//      CreateDeferredContext (log:334) y NO DIBUJA con ese contexto. Toda la
//      geometria de las tres capturas salio por el contexto inmediato: no
//      aparecio ni una subcarpeta ctx-0x* dentro de ningun FrameAnalysis-*, que
//      es la senal que 3Dmigoto deja cuando el que volca es un diferido. En el
//      inmediato los Get* SI son fiables, asi que hoy esto no muerde.
//   2. No hay alternativa barata. La variable rt_width del ini NO sirve:
//      ProcessParamRTSize mira la VISTA y vale -1 para cualquier dimension que
//      no sea TEXTURE2D/TEXTURE2DMS (arrays incluidos). Esta funcion mira el
//      RECURSO, que es lo unico correcto para todas las dimensiones de vista.
//   3. Si el estado se trackeara en el wrapper (lo que pide la regla dura 3),
//      habria que interceptar OMSetRenderTargets y llevar una copia por
//      contexto. Es lo correcto y NO esta hecho.
//
// CUANDO VUELVE A MORDER: si aparece un pase dibujado por el contexto diferido.
// El unico sitio donde puede pasar y no esta medido es la REPETICION (la sesion
// 01 solo capturo menu y cockpit); la captura 9 del PROTOCOLO-SESION-02 mide
// exactamente eso. Sintoma esperado si pasa: el scissor por tercio sale mal SOLO
// en algunos pases.
// (Hallazgo 12d de la revision adversarial de la sesion 01.)
static bool triple_get_rt_size(CommandListState *state, float *w, float *h)
{
	ID3D11RenderTargetView *view = NULL;
	ID3D11Resource *res = NULL;
	D3D11_RESOURCE_DIMENSION dim;
	bool ok = false;

	state->mOrigContext1->OMGetRenderTargets(1, &view, NULL);
	if (!view)
		return false;
	view->GetResource(&res);
	view->Release();
	if (!res)
		return false;

	res->GetType(&dim);
	if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d;
		((ID3D11Texture2D*)res)->GetDesc(&d);
		*w = (float)d.Width;
		*h = (float)d.Height;
		ok = (d.Width > 0);
	}
	res->Release();
	return ok;
}

// Construye P_region (view-space del juego -> clip de la region i) desde la
// geometria viva (constantes K en IniParams) y el near REAL de la camara.
// Deducida de tools/compute_matrices.py; equivale exactamente a lo que hace el
// parche ASM:
//   laterales: x = KA*Xv + s*KB*Zv ; y = KC*Yv ; w = s*sa*Xv - ca*Zv ; z = w - near
//   centro:    x = KXc*Xv ; y = KYc*Yv ; w = -Zv ; z = w - near
// con s = -1 para IZQ y +1 para DER (Zv = -w, de ahi los signos).
static void triple_build_pregion(float *P, int region, float nearz)
{
	memset(P, 0, 16 * sizeof(float));
	if (region == 1) {                 // CENTRO
		P[0]  = TRIPLE_KXC;            // col0.row0
		P[5]  = TRIPLE_KYC;            // col1.row1
		P[10] = -1.0f;                 // col2.row2
		P[11] = -1.0f;                 // col2.row3
		P[14] = -nearz;                // col3.row2
	} else {
		float s = (region == 0) ? -1.0f : 1.0f;
		P[0]  = TRIPLE_KA;             // col0.row0
		P[2]  = s * TRIPLE_SA;         // col0.row2
		P[3]  = s * TRIPLE_SA;         // col0.row3
		P[5]  = TRIPLE_KC;             // col1.row1
		P[8]  = s * TRIPLE_KB;         // col2.row0
		P[10] = -TRIPLE_CA;            // col2.row2
		P[11] = -TRIPLE_CA;            // col2.row3
		P[14] = -nearz;                // col3.row2
	}
}

void TripleRegionCommand::run(CommandListState *state)
{
	ID3D11DeviceContext1 *ctx = state->mOrigContext1;
	ID3D11Device1 *dev = state->mOrigDevice1;

	// triple_scissor = restore: reponer los scissor rects del JUEGO desde la
	// copia sombra. Si el juego no habia seteado ninguno, dejamos el rect neutro
	// de 16384 que no puede recortar nada (regla dura 1).
	if (scissor_only == 2) {
		HackerContext *hc = state->mHackerContext;
		if (hc && hc->mTripleScissorShadowCount > 0) {
			ctx->RSSetScissorRects(hc->mTripleScissorShadowCount, hc->mTripleScissorShadow);
		} else {
			D3D11_RECT r;
			r.left = 0; r.top = 0; r.right = 16384; r.bottom = 16384;
			ctx->RSSetScissorRects(1, &r);
		}
		return;
	}

	// triple_scissor: solo el rectangulo del tercio, como FRACCION del render
	// target real. Independiente de la resolucion. No toca ningun cbuffer.
	if (scissor_only) {
		float rw = 0.0f, rh = 0.0f;
		if (region >= 0 && region < 3 && triple_get_rt_size(state, &rw, &rh)) {
			float flo, fhi;
			triple_get_frac(region, &flo, &fhi);
			D3D11_RECT r;
			r.left   = (LONG)(rw * flo + 0.5f);
			r.right  = (LONG)(rw * fhi + 0.5f);
			r.top    = 0;
			r.bottom = (LONG)(rh + 0.5f);
			ctx->RSSetScissorRects(1, &r);
		}
		return;
	}

	// Se loguea UNA vez por cada combinacion distinta de (id de pase, tamano de
	// RT), asi se ve exactamente CUALES pases se estan corrigiendo, a que
	// resolucion, y con que near de camara - sin inundar el log.
	if (region == 0 && G->iniParams.size() >= 4) {
		float lw = -1.0f, lh = -1.0f;
		triple_get_rt_size(state, &lw, &lh);
		int id = (int)G->iniParams[3].z;
		const float *cf = (const float*)g_triple_cam_buf;
		static int seen[32] = { 0 };
		static int nseen = 0;
		int key = id * 100000 + (int)lw;
		bool nuevo = true;
		for (int k = 0; k < nseen; k++) if (seen[k] == key) { nuevo = false; break; }
		if (nuevo && nseen < 32) {
			seen[nseen++] = key;
			LogInfo("TRIPLE pase #%d: rt=%.0fx%.0f  near_cam=%.4f m00=%.5f m11=%.5f  cam_valid=%d\n",
				id, lw, lh, g_triple_cam_valid ? -cf[14] : 0.0f,
				g_triple_cam_valid ? cf[0] : 0.0f, g_triple_cam_valid ? cf[5] : 0.0f,
				(int)g_triple_cam_valid);
		}
	}

	if (!g_triple_cam_valid)
		return;  // todavia no se capturo la camara principal

	// NADA de Get* aca (regla dura 3). Para restaurar usamos g_triple_orig_cb,
	// una copia sin modificar de la camara capturada.

	// Recalcular los 3 cbs de region como maximo una vez por frame. Aca solo se
	// hace UpdateSubresource / bind, las dos operaciones validas en un contexto
	// diferido (regla dura 2: nunca actualizar un recurso GPU entre draws).
	if (g_triple_frame != G->frame_no) {
		if (g_triple_cb_size != g_triple_cam_size) {
			for (int i = 0; i < 3; i++) {
				if (g_triple_region_cb[i]) { g_triple_region_cb[i]->Release(); g_triple_region_cb[i] = NULL; }
			}
			if (g_triple_orig_cb) { g_triple_orig_cb->Release(); g_triple_orig_cb = NULL; }
			g_triple_cb_size = g_triple_cam_size;

			D3D11_BUFFER_DESC rd;
			rd.ByteWidth = g_triple_cam_size;
			rd.Usage = D3D11_USAGE_DEFAULT;
			rd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			rd.CPUAccessFlags = 0;
			rd.MiscFlags = 0;
			rd.StructureByteStride = 0;
			for (int i = 0; i < 3; i++)
				dev->CreateBuffer(&rd, NULL, &g_triple_region_cb[i]);
			dev->CreateBuffer(&rd, NULL, &g_triple_orig_cb);
		}

		if (g_triple_region_cb[0] && G->iniParams.size() >= 3) {
			const float *f = (const float*)g_triple_cam_buf;
			const float *Pg   = f;         // projection
			const float *IPg  = f + 16;    // inverseProj
			const float *RVPg = f + 36;    // relativeViewProjection
			float nearz = -f[14];              // m23 = -near
			if (nearz <= 0.0f) nearz = 0.1f;   // guarda

			float Pgi[16];
			bool ok_pg = triple_mat_inverse(Pgi, Pg);

			// Copia intacta para restaurar sin Get*.
			if (g_triple_orig_cb)
				ctx->UpdateSubresource(g_triple_orig_cb, 0, NULL, g_triple_cam_buf, 0, 0);

			for (int i = 0; i < 3 && ok_pg; i++) {
				// TrueTriple DR1: alignas(16). En DR2 era `char rbuf[512]` y se
				// castea a float*: en x86 un array de char local no tiene
				// garantia de alineacion a 4.
				alignas(16) char rbuf[512];
				memcpy(rbuf, g_triple_cam_buf, g_triple_cam_size);
				float *rf = (float*)rbuf;

				float Preg[16], Pregi[16], Mi[16], Mii[16], tmp[16];
				triple_build_pregion(Preg, i, nearz);
				if (!triple_mat_inverse(Pregi, Preg))
					continue;

				// REGLA UNIVERSAL (no depende de que mapea cada matriz):
				// nuestra correccion en el shader equivale a
				// clip_i = M_i * clip_juego, con M_i = P_region_i * inv(P_juego).
				// Entonces:
				//   toda matriz que produce CLIP  -> se premultiplica por M_i
				//   toda matriz que consume CLIP  -> se posmultiplica por inv(M_i)
				triple_mat_mul(Mi,  Preg, Pgi);    // M_i      = P_region * inv(P_juego)
				triple_mat_mul(Mii, Pg,   Pregi);  // inv(M_i) = P_juego  * inv(P_region)

				// SOLO se reemplaza inverseProj. projection y
				// relativeViewProjection se dejan INTACTAS. Es esencial:
				//  - el parche ASM del vertex shader calcula la correccion
				//    leyendo cb3[9..12] (relativeViewProjection). Si tambien la
				//    cambiaramos, aplicaria la correccion DOS VECES.
				//  - para reconstruir posicion desde el depth alcanza con
				//    inverseProj, que es lo unico que leen esos pixel shaders.
				// Radio de dano minimo: si la camara capturada fuera la
				// equivocada, solo se degrada la reconstruccion, nunca la
				// geometria.
				(void)Preg; (void)Mi; (void)RVPg;
				triple_mat_mul(tmp, IPg, Mii);     // inverseProj_i = inverseProj * inv(M_i)
				memcpy(rf + 16, tmp, 64);

				ctx->UpdateSubresource(g_triple_region_cb[i], 0, NULL, rbuf, 0, 0);

				static int trdbg = 0;
				if (trdbg < 3) {
					LogInfo("TRIPLE region %d: near=%.6f KA=%.5f KB=%.5f KC=%.5f KXc=%.5f KYc=%.5f sa=%.5f ca=%.5f\n",
						i, nearz, TRIPLE_KA, TRIPLE_KB, TRIPLE_KC,
						TRIPLE_KXC, TRIPLE_KYC, TRIPLE_SA, TRIPLE_CA);
					LogInfo("            M_i col0=(%.4f %.4f %.4f %.4f) invM col0=(%.4f %.4f %.4f %.4f)\n",
						Mi[0], Mi[1], Mi[2], Mi[3], Mii[0], Mii[1], Mii[2], Mii[3]);
					trdbg++;
				}
			}
			g_triple_valid = ok_pg;
			g_triple_frame = G->frame_no;
		}
	}

	if (g_triple_valid && region >= 0 && region < 3 && g_triple_region_cb[region]) {
		// CERRADO por la sesion 01: NO hace falta el DSSetConstantBuffers.
		// El TODO preguntaba si algun DOMAIN shader reconstruye posicion desde
		// inverseProj (los 4 ds que bindean b3, con cloth+water tessellation en
		// true). La respuesta esta medida: analisis\sesion01\desproyectores.json
		// tiene los 18 desproyectores del juego y son 13 ps + 5 cs. CERO vs, ds
		// o hs. Ningun domain shader desproyecta, asi que bindear la region a la
		// etapa DS no le serviria a nadie.
		// Ojo con no confundir dos cosas distintas: BINDEAR b3 no es DESPROYECTAR
		// desde inverseProj. Un DS puede leer eyePositionWS de b3 y proyectar con
		// su propia modelViewProj -que es exactamente lo que hacen los 10 ds de
		// la familia cb0[4..7]-. La medicion es sobre el ENSAMBLADOR (que entra
		// en la cadena que reconstruye la posicion), no sobre el conteo de bindeos.
		// Si algun dia aparece un ds desproyector, esta linea es el lugar.
		// (Hallazgo 12a de la revision adversarial de la sesion 01.)
		ctx->VSSetConstantBuffers(3, 1, &g_triple_region_cb[region]);
		ctx->PSSetConstantBuffers(3, 1, &g_triple_region_cb[region]);

		// Scissor PROPORCIONAL al render target real de este pase. Asi funciona
		// tanto a resolucion completa como a media sin hardcodear pixeles.
		float rw = 0.0f, rh = 0.0f;
		if (triple_get_rt_size(state, &rw, &rh)) {
			float flo, fhi;
			triple_get_frac(region, &flo, &fhi);
			D3D11_RECT r;
			r.left   = (LONG)(rw * flo + 0.5f);
			r.right  = (LONG)(rw * fhi + 0.5f);
			r.top    = 0;
			r.bottom = (LONG)(rh + 0.5f);
			ctx->RSSetScissorRects(1, &r);

			static int trrt = 0;
			if (trrt < 6) {
				LogInfo("TRIPLE fullscreen pass: rt=%.0fx%.0f region=%d scissor=%ld..%ld\n",
					rw, rh, region, r.left, r.right);
				trrt++;
			}
		}
	}
}

// ---- triple_params: texturas de IniParams pre-armadas por region ------------
// 4 juegos (region 0/1/2 y 3 = passthrough), POR CONTEXTO (thread_local, regla
// dura 5). Se refrescan una vez por frame desde G->iniParams - el mismo
// contenido que subiria el flush normal - con la unica diferencia de que
// row0.x = region fija. Bindear un SRV es casi gratis; hacer un
// Map(WRITE_DISCARD) de IniParams antes de CADA redibujo costaba ~10 ms/frame
// (regla dura 2).
static thread_local ID3D11Texture1D *g_triple_par_tex[4] = { NULL, NULL, NULL, NULL };
static thread_local ID3D11ShaderResourceView *g_triple_par_srv[4] = { NULL, NULL, NULL, NULL };
static thread_local UINT g_triple_par_size = 0;
static thread_local unsigned g_triple_par_frame = 0xFFFFFFFF;

void TripleParamsCommand::run(CommandListState *state)
{
	ID3D11DeviceContext1 *ctx = state->mOrigContext1;
	ID3D11Device1 *dev = state->mOrigDevice1;
	UINT n = (UINT)G->iniParams.size();

	if (!n || G->IniParamsReg < 0)
		return;

	if (g_triple_par_size != n) {
		for (int i = 0; i < 4; i++) {
			if (g_triple_par_srv[i]) { g_triple_par_srv[i]->Release(); g_triple_par_srv[i] = NULL; }
			if (g_triple_par_tex[i]) { g_triple_par_tex[i]->Release(); g_triple_par_tex[i] = NULL; }
		}
		g_triple_par_size = n;

		D3D11_TEXTURE1D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Width = n;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		for (int i = 0; i < 4; i++) {
			if (FAILED(dev->CreateTexture1D(&desc, NULL, &g_triple_par_tex[i])))
				return;
			if (FAILED(dev->CreateShaderResourceView(g_triple_par_tex[i], NULL, &g_triple_par_srv[i]))) {
				g_triple_par_tex[i]->Release(); g_triple_par_tex[i] = NULL;
				return;
			}
		}
		g_triple_par_frame = 0xFFFFFFFF;  // forzar refresco
	}

	if (g_triple_par_frame != G->frame_no && g_triple_par_tex[0]) {
		std::vector<DirectX::XMFLOAT4> buf(G->iniParams);
		for (int i = 0; i < 4; i++) {
			buf[0].x = (float)i;  // 0,1,2 = region ; 3 = passthrough
			// n * 16: cada fila es un XMFLOAT4 = 16 bytes. UINT alcanza de sobra
			// (n es la cantidad de filas de IniParams), no hay overflow en x86.
			ctx->UpdateSubresource(g_triple_par_tex[i], 0, NULL, buf.data(), n * 16, 0);
		}
		g_triple_par_frame = G->frame_no;
	}

	// y3 == 1 solo en triple normal ($triple_on == 1, lo publica [Present]).
	// En diagnostico (2) o apagado se bindea la passthrough (x = 3).
	int idx = region;
	if (n > 3 && G->iniParams[3].y < 0.5f)
		idx = 3;

	// CONTADOR DE DIAGNOSTICO (portado de DR2, donde se agrego el 2026-08-07
	// resolviendo "el triple no corrige en cockpit/capo/piso").
	//
	// Este es EL contador que separa las DOS causas posibles de "se ve como
	// vanilla", que en pantalla son IDENTICAS y en el log tambien:
	//   - r0/r1/r2 en ~0 mientras se maneja -> el command list del redibujo NO
	//     CORRE (el gate de rt_width / $triple_on nunca se cumple);
	//   - r0/r1/r2 altos pero todo cayendo en passthrough -> la maquinaria SI
	//     corre, pero bindea la fila 3 porque y3 esta en 0 (triple apagado o en
	//     modo diagnostico).
	// Sin esta linea las dos hipotesis cuestan una sesion de pruebas cada una.
	//
	// Contadores globales SIN CANDADO a proposito: es diagnostico puro. Una
	// carrera entre contextos pierde alguna cuenta y no cambia la conclusion
	// (lo que se lee es "cero contra miles", no el numero exacto). Poner un
	// candado por draw costaria mas que el problema que evita.
	//
	// PORTABILIDAD x86: no hay nada dependiente de arquitectura aca (unsigned
	// de 32 bits, un %u por contador y un %.0f). A diferencia del volcado de
	// pila de DR2, que leia Rip/Rsp/Rax y por eso NO se porto a Win32.
	{
		static unsigned cnt[4];
		static unsigned ultimo_frame = 0;
		if (idx >= 0 && idx <= 3)
			cnt[idx]++;
		if (G->frame_no - ultimo_frame >= 600) {
			ultimo_frame = G->frame_no;
			LogInfo("TRIPLE params (ultimos ~600 frames): r0=%u r1=%u r2=%u"
				" passthrough=%u  y3=%.0f\n",
				cnt[0], cnt[1], cnt[2], cnt[3],
				(n > 3) ? G->iniParams[3].y : -1.0f);
			cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
		}
	}

	if (g_triple_par_srv[idx]) {
		ctx->VSSetShaderResources((UINT)G->IniParamsReg, 1, &g_triple_par_srv[idx]);
		ctx->DSSetShaderResources((UINT)G->IniParamsReg, 1, &g_triple_par_srv[idx]);
	}
}

void TripleRestoreCommand::run(CommandListState *state)
{
	// Rebind de la copia INTACTA de la camara (mismo contenido que tenia el
	// juego en este frame). No usamos Get*/save por ser inseguro en contextos
	// diferidos.
	if (g_triple_valid && g_triple_orig_cb) {
		state->mOrigContext1->VSSetConstantBuffers(3, 1, &g_triple_orig_cb);
		state->mOrigContext1->PSSetConstantBuffers(3, 1, &g_triple_orig_cb);
	}
	// Restaurar los rects del JUEGO (copia sombra); si no habia, el rect neutro
	// de 16384 que no puede recortar nada (regla dura 1: dejar siempre un valor
	// que no pueda hacer dano; 16384 = maximo de textura D3D11).
	{
		HackerContext *hc = state->mHackerContext;
		if (hc && hc->mTripleScissorShadowCount > 0) {
			state->mOrigContext1->RSSetScissorRects(hc->mTripleScissorShadowCount, hc->mTripleScissorShadow);
		} else {
			D3D11_RECT r;
			r.left = 0; r.top = 0; r.right = 16384; r.bottom = 16384;
			state->mOrigContext1->RSSetScissorRects(1, &r);
		}
	}
}

// ===== fin TrueTriple DR1 ===================================================

PerDrawStereoOverrideCommand::PerDrawStereoOverrideCommand(bool restore_on_post) :
		staging_type(false),
		val(FLT_MAX),
		saved(FLT_MAX),
		restore_on_post(restore_on_post),
		did_set_value_on_pre(false)
{}

bool PerDrawStereoOverrideCommand::update_val(CommandListState *state)
{
	D3D11_MAPPED_SUBRESOURCE mapping;
	HRESULT hr;
	float tmp;
	bool ret = false;

	if (!staging_type)
		return true;

	if (staging_op.staging) {
		hr = staging_op.map(state, &mapping);
		if (FAILED(hr)) {
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
				COMMAND_LIST_LOG(state, "  Transfer in progress...\n");
			else
				COMMAND_LIST_LOG(state, "  Unknown error: 0x%x\n", hr);
			return false;
		}

		// FIXME: Check if resource is at least 4 bytes (maybe we can
		// use RowPitch, but MSDN contradicts itself so I'm not sure.
		// Otherwise we can refer to the resource description)
		tmp = ((float*)mapping.pData)[0];
		staging_op.unmap(state);

		if (isnan(tmp)) {
			COMMAND_LIST_LOG(state, "  Disregarding NAN\n");
		} else {
			val = tmp;
			ret = true;
		}

		// To make auto-convergence as responsive as possible, we start
		// the next transfer as soon as we have retrieved the value
		// from the previous transfer. This should minimise the number
		// of frames displayed with wrong convergence on scene changes.
	}

	staging_op.staging = true;
	staging_op.run(state);
	return ret;
}

void PerDrawStereoOverrideCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (!state->mHackerDevice->mStereoHandle) {
		COMMAND_LIST_LOG(state, "  No Stereo Handle\n");
		return;
	}

	if (restore_on_post) {
		if (state->post) {
			if (!did_set_value_on_pre)
				return;
			did_set_value_on_pre = false;

			COMMAND_LIST_LOG(state, "  Restoring %s = %f\n", stereo_param_name(), saved);
			set_stereo_value(state, saved);
		} else {
			if (staging_type) {
				if (!(did_set_value_on_pre = update_val(state)))
					return;
			} else {
				val = expression.evaluate(state);
				did_set_value_on_pre = true;
			}

			saved = get_stereo_value(state);

			COMMAND_LIST_LOG(state, "  Setting per-draw call %s = %f * %f = %f\n",
					stereo_param_name(), val, saved, val * saved);

			// The original ShaderOverride code multiplied the new
			// separation and convergence by the old ones, so I'm
			// doing that as well, but while that makes sense for
			// separation, I'm not really convinced it makes sense
			// for convergence. Still, the convergence override is
			// generally only useful to use convergence=0 to move
			// something to infinity, and in that case it won't
			// matter.
			set_stereo_value(state, val * saved);
		}
	} else {
		if (staging_type) {
			if (!update_val(state))
				return;
		} else
			val = expression.evaluate(state);

		COMMAND_LIST_LOG(state, "  Setting %s = %f\n", stereo_param_name(), val);
		set_stereo_value(state, val);
	}
}

bool PerDrawStereoOverrideCommand::optimise(HackerDevice *device)
{
	if (staging_type)
		return false;
	return expression.optimise(device);
}

bool PerDrawStereoOverrideCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	NvU8 enabled = false;

	NvAPIOverride();
	Profiling::NvAPI_Stereo_IsEnabled(&enabled);
	return !enabled;
}

void DirectModeSetActiveEyeCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (NVAPI_OK != Profiling::NvAPI_Stereo_SetActiveEye(state->mHackerDevice->mStereoHandle, eye))
		COMMAND_LIST_LOG(state, "  Stereo_SetActiveEye failed\n");
}

bool DirectModeSetActiveEyeCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	NvU8 enabled = false;

	NvAPIOverride();
	Profiling::NvAPI_Stereo_IsEnabled(&enabled);
	return !enabled;

	// FIXME: Should also return false if direct mode is disabled...
	// if only nvapi provided a GetDriverMode() API to determine that
}

float PerDrawSeparationOverrideCommand::get_stereo_value(CommandListState *state)
{
	float ret = 0.0f;

	if (NVAPI_OK != Profiling::NvAPI_Stereo_GetSeparation(state->mHackerDevice->mStereoHandle, &ret))
		COMMAND_LIST_LOG(state, "  Stereo_GetSeparation failed\n");

	return ret;
}

void PerDrawSeparationOverrideCommand::set_stereo_value(CommandListState *state, float val)
{
	NvAPIOverride();
	if (NVAPI_OK != Profiling::NvAPI_Stereo_SetSeparation(state->mHackerDevice->mStereoHandle, val))
		COMMAND_LIST_LOG(state, "  Stereo_SetSeparation failed\n");
}

float PerDrawConvergenceOverrideCommand::get_stereo_value(CommandListState *state)
{
	float ret = 0.0f;

	if (NVAPI_OK != Profiling::NvAPI_Stereo_GetConvergence(state->mHackerDevice->mStereoHandle, &ret))
		COMMAND_LIST_LOG(state, "  Stereo_GetConvergence failed\n");

	return ret;
}

void PerDrawConvergenceOverrideCommand::set_stereo_value(CommandListState *state, float val)
{
	NvAPIOverride();
	if (NVAPI_OK != Profiling::NvAPI_Stereo_SetConvergence(state->mHackerDevice->mStereoHandle, val))
		COMMAND_LIST_LOG(state, "  Stereo_SetConvergence failed\n");
}

FrameAnalysisChangeOptionsCommand::FrameAnalysisChangeOptionsCommand(wstring *val)
{
	wchar_t *buf;
	size_t size = val->size() + 1;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	buf = new wchar_t[size];
	wcscpy_s(buf, size, val->c_str());

	analyse_options = parse_enum_option_string<wchar_t *, FrameAnalysisOptions>
		(FrameAnalysisOptionNames, buf, NULL);

	delete [] buf;
}

void FrameAnalysisChangeOptionsCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	state->mHackerContext->FrameAnalysisTrigger(analyse_options);
}

bool FrameAnalysisChangeOptionsCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	return (G->hunting == HUNTING_MODE_DISABLED || G->frame_analysis_registered == false);
}

static void FillInMissingInfo(ResourceCopyTargetType type, ID3D11Resource *resource, ID3D11View *view,
		UINT *stride, UINT *offset, UINT *buf_size, DXGI_FORMAT *format)
{
	D3D11_RESOURCE_DIMENSION dimension;
	D3D11_BUFFER_DESC buf_desc;
	ID3D11Buffer *buffer;

	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view = NULL;
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;

	D3D11_SHADER_RESOURCE_VIEW_DESC resource_view_desc;
	D3D11_RENDER_TARGET_VIEW_DESC render_view_desc;
	D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc;
	D3D11_UNORDERED_ACCESS_VIEW_DESC unordered_view_desc;

	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;

	// Some of these may already be filled in when getting the resource
	// (either because it is stored in the pipeline state and retrieved
	// with the resource, or was stored in a custom resource). If they are
	// not we will try to fill them in here from either the resource or
	// view description as they may be necessary later to create a
	// compatible view or perform a region copy:

	resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
		buffer = (ID3D11Buffer*)resource;
		buffer->GetDesc(&buf_desc);
		if (*buf_size)
			*buf_size = min(*buf_size, buf_desc.ByteWidth);
		else
			*buf_size = buf_desc.ByteWidth;

		if (!*stride)
			*stride = buf_desc.StructureByteStride;
	}

	if (view) {
		switch (type) {
			case ResourceCopyTargetType::SHADER_RESOURCE:
				resource_view = (ID3D11ShaderResourceView*)view;
				resource_view->GetDesc(&resource_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = resource_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = resource_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = resource_view_desc.Buffer.NumElements * *stride + *offset;
				break;
			case ResourceCopyTargetType::RENDER_TARGET:
				render_view = (ID3D11RenderTargetView*)view;
				render_view->GetDesc(&render_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = render_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = render_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = render_view_desc.Buffer.NumElements * *stride + *offset;
				break;
			case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
				depth_view = (ID3D11DepthStencilView*)view;
				depth_view->GetDesc(&depth_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = depth_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				// Depth stencil buffers cannot be buffers
				break;
			case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
				unordered_view = (ID3D11UnorderedAccessView*)view;
				unordered_view->GetDesc(&unordered_view_desc);
				if (*format == DXGI_FORMAT_UNKNOWN)
					*format = unordered_view_desc.Format;
				if (!*stride)
					*stride = dxgi_format_size(*format);
				if (!*offset)
					*offset = unordered_view_desc.Buffer.FirstElement * *stride;
				if (!*buf_size)
					*buf_size = unordered_view_desc.Buffer.NumElements * *stride + *offset;
				break;
		}
	} else if (*format == DXGI_FORMAT_UNKNOWN) {
		// If we *still* don't know the format and it's a texture, get it from
		// the resource description. This will be the case for the back buffer
		// since that does not have a view.
		switch (dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				tex1d = (ID3D11Texture1D*)resource;
				tex1d->GetDesc(&tex1d_desc);
				*format = tex1d_desc.Format;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				tex2d = (ID3D11Texture2D*)resource;
				tex2d->GetDesc(&tex2d_desc);
				*format = tex2d_desc.Format;
				break;
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				tex3d = (ID3D11Texture3D*)resource;
				tex3d->GetDesc(&tex3d_desc);
				*format = tex3d_desc.Format;
		}
	}

	if (!*stride) {
		// This will catch index buffers, which are not structured and
		// don't have a view, but they do have a format we can use:
		*stride = dxgi_format_size(*format);

		// This will catch constant buffers, which are not structured
		// and don't have either a view or format, so set the stride to
		// the size of the whole buffer:
		if (!*stride)
			*stride = *buf_size;
	}
}

void FrameAnalysisDumpCommand::run(CommandListState *state)
{
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	UINT stride = 0;
	UINT offset = 0;
	UINT buf_size = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	// Fast exit if frame analysis is currently inactive:
	if (!G->analyse_frame)
		return;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	resource = target.GetResource(state, &view, &stride, &offset, &format, NULL);
	if (!resource) {
		COMMAND_LIST_LOG(state, "  No resource to dump\n");
		return;
	}

	// Fill in any missing info before handing it to frame analysis. The
	// format is particularly important to try to avoid saving TYPELESS
	// resources:
	FillInMissingInfo(target.type, resource, view, &stride, &offset, &buf_size, &format);

	state->mHackerContext->FrameAnalysisDump(resource, analyse_options, target_name.c_str(), format, stride, offset);

	if (resource)
		resource->Release();
	if (view)
		view->Release();
}

bool FrameAnalysisDumpCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	return (G->hunting == HUNTING_MODE_DISABLED || G->frame_analysis_registered == false);
}

UpscalingFlipBBCommand::UpscalingFlipBBCommand(wstring section) :
	ini_section(section)
{
	G->upscaling_command_list_using_explicit_bb_flip = true;
}

UpscalingFlipBBCommand::~UpscalingFlipBBCommand()
{
	G->upscaling_command_list_using_explicit_bb_flip = false;
}

void UpscalingFlipBBCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] special = upscaling_switch_bb\n", ini_section.c_str());

	G->bb_is_upscaling_bb = false;
}

void Draw3DMigotoOverlayCommand::run(CommandListState *state)
{
	COMMAND_LIST_LOG(state, "[%S] special = draw_3dmigoto_overlay\n", ini_section.c_str());

	HackerSwapChain *mHackerSwapChain = state->mHackerDevice->GetHackerSwapChain();
	if (mHackerSwapChain->mOverlay) {
		mHackerSwapChain->mOverlay->DrawOverlay();
		G->suppress_overlay = true;
	}
}

CustomShader::CustomShader() :
	vs_override(false), hs_override(false), ds_override(false),
	gs_override(false), ps_override(false), cs_override(false),
	vs(NULL), hs(NULL), ds(NULL), gs(NULL), ps(NULL), cs(NULL),
	vs_bytecode(NULL), hs_bytecode(NULL), ds_bytecode(NULL),
	gs_bytecode(NULL), ps_bytecode(NULL), cs_bytecode(NULL),
	blend_override(0), blend_state(NULL),
	blend_sample_mask(0xffffffff), blend_sample_mask_merge_mask(0xffffffff),
	depth_stencil_override(0), depth_stencil_state(NULL),
	stencil_ref(0), stencil_ref_mask(~0),
	rs_override(0), rs_state(NULL),
	topology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
	substantiated(false),
	max_executions_per_frame(0),
	frame_no(0),
	executions_this_frame(0),
	sampler_override(0),
	sampler_state(nullptr),
	compile_flags(D3DCompileFlags::OPTIMIZATION_LEVEL3)
{
	int i;

	for (i = 0; i < 4; i++) {
		blend_factor[i] = 1.0f;
		blend_factor_merge_mask[i] = ~0;
	}
}

CustomShader::~CustomShader()
{
	if (vs)
		vs->Release();
	if (hs)
		hs->Release();
	if (ds)
		ds->Release();
	if (gs)
		gs->Release();
	if (ps)
		ps->Release();
	if (cs)
		cs->Release();

	if (blend_state)
		blend_state->Release();
	if (depth_stencil_state)
		depth_stencil_state->Release();
	if (rs_state)
		rs_state->Release();

	if (vs_bytecode)
		vs_bytecode->Release();
	if (hs_bytecode)
		hs_bytecode->Release();
	if (ds_bytecode)
		ds_bytecode->Release();
	if (gs_bytecode)
		gs_bytecode->Release();
	if (ps_bytecode)
		ps_bytecode->Release();
	if (cs_bytecode)
		cs_bytecode->Release();
	if (sampler_state)
		sampler_state->Release();
}

static bool load_cached_shader(FILETIME hlsl_timestamp, wchar_t *cache_path, ID3DBlob **ppBytecode)
{
	FILETIME cache_timestamp;
	HANDLE f_cache;
	DWORD filesize, readsize;

	f_cache = CreateFile(cache_path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f_cache == INVALID_HANDLE_VALUE)
		return false;

	if (!GetFileTime(f_cache, NULL, NULL, &cache_timestamp)
	 || CompareFileTime(&hlsl_timestamp, &cache_timestamp)) {
		LogInfo("    Discarding stale cached shader: %S\n", cache_path);
		goto err_close;
	}

	filesize = GetFileSize(f_cache, 0);
	if (FAILED(D3DCreateBlob(filesize, ppBytecode))) {
		LogInfo("    D3DCreateBlob failed\n");
		goto err_close;
	}

	if (!ReadFile(f_cache, (*ppBytecode)->GetBufferPointer(), (DWORD)(*ppBytecode)->GetBufferSize(), &readsize, 0)
			|| readsize != filesize) {
		LogInfo("    Error reading cached shader\n");
		goto err_free;
	}

	LogInfo("    Loaded cached shader: %S\n", cache_path);
	CloseHandle(f_cache);
	return true;

err_free:
	(*ppBytecode)->Release();
	*ppBytecode = NULL;
err_close:
	CloseHandle(f_cache);
	return false;
}

static const D3D_SHADER_MACRO vs_macros[] = { "VERTEX_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO hs_macros[] = { "HULL_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO ds_macros[] = { "DOMAIN_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO gs_macros[] = { "GEOMETRY_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO ps_macros[] = { "PIXEL_SHADER", "", NULL, NULL };
static const D3D_SHADER_MACRO cs_macros[] = { "COMPUTE_SHADER", "", NULL, NULL };

// This is similar to the other compile routines, but still distinct enough to
// get it's own function for now - TODO: Refactor out the common code
bool CustomShader::compile(char type, wchar_t *filename, const wstring *wname, const wstring *namespace_path)
{
	wchar_t wpath[MAX_PATH], cache_path[MAX_PATH];
	char apath[MAX_PATH];
	HANDLE f;
	DWORD srcDataSize, readSize;
	vector<char> srcData;
	HRESULT hr;
	char shaderModel[7];
	ID3DBlob **ppBytecode = NULL;
	ID3DBlob *pErrorMsgs = NULL;
	const D3D_SHADER_MACRO *macros = NULL;
	bool found = false;
	FILETIME timestamp;

	LogInfo("  %cs=%S\n", type, filename);

	switch(type) {
		case 'v':
			ppBytecode = &vs_bytecode;
			macros = vs_macros;
			vs_override = true;
			break;
		case 'h':
			ppBytecode = &hs_bytecode;
			macros = hs_macros;
			hs_override = true;
			break;
		case 'd':
			ppBytecode = &ds_bytecode;
			macros = ds_macros;
			ds_override = true;
			break;
		case 'g':
			ppBytecode = &gs_bytecode;
			macros = gs_macros;
			gs_override = true;
			break;
		case 'p':
			ppBytecode = &ps_bytecode;
			macros = ps_macros;
			ps_override = true;
			break;
		case 'c':
			ppBytecode = &cs_bytecode;
			macros = cs_macros;
			cs_override = true;
			break;
		default:
			// Should not happen
			LogOverlay(LOG_DIRE, "CustomShader::compile: invalid shader type\n");
			goto err;
	}

	// Special value to unbind the shader instead:
	if (!_wcsicmp(filename, L"null"))
		return false;

	// If this section was not in the main d3dx.ini, look
	// for a file relative to the config it came from
	// first, then try relative to the 3DMigoto directory:
	found = false;
	if (!namespace_path->empty()) {
		GetModuleFileName(migoto_handle, wpath, MAX_PATH);
		wcsrchr(wpath, L'\\')[1] = 0;
		wcscat(wpath, namespace_path->c_str());
		wcscat(wpath, filename);
		if (GetFileAttributes(wpath) != INVALID_FILE_ATTRIBUTES)
			found = true;
	}
	if (!found) {
		if (!GetModuleFileName(migoto_handle, wpath, MAX_PATH)) {
			LogOverlay(LOG_DIRE, "CustomShader::compile: GetModuleFileName failed\n");
			goto err;
		}
		wcsrchr(wpath, L'\\')[1] = 0;
		wcscat(wpath, filename);
	}

	f = CreateFile(wpath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogOverlay(LOG_WARNING, "Shader not found: %S\n", wpath);
		goto err;
	}

	// Currently always using shader model 5, could allow this to be
	// overridden in the future:
	_snprintf_s(shaderModel, 7, 7, "%cs_5_0", type);

	// XXX: If we allow the compilation to be customised further (e.g. with
	// addition preprocessor defines), make the cache filename unique for
	// each possible combination
	wchar_t *ext = wcsrchr(wpath, L'.');
	if (ext > wcsrchr(wpath, L'\\'))
		swprintf_s(cache_path, MAX_PATH, L"%.*s.%S.%x.bin", (int)(ext - wpath), wpath, shaderModel, (UINT)compile_flags);
	else
		swprintf_s(cache_path, MAX_PATH, L"%s.%S.%x.bin", wpath, shaderModel, (UINT)compile_flags);

	GetFileTime(f, NULL, NULL, &timestamp);
	if (load_cached_shader(timestamp, cache_path, ppBytecode)) {
		CloseHandle(f);
		return false;
	}

	srcDataSize = GetFileSize(f, 0);
	srcData.resize(srcDataSize);

	if (!ReadFile(f, srcData.data(), srcDataSize, &readSize, 0)
			|| srcDataSize != readSize) {
		LogInfo("    Error reading HLSL file\n");
		goto err_close;
	}
	CloseHandle(f);

	// TODO: Add #defines for StereoParams and IniParams. Define a macro
	// for the type of shader, and maybe allow more defines to be specified
	// in the ini

	// Pass the real filename and use the standard include handler so that
	// #include will work with a relative path from the shader itself.
	// Later we could add a custom include handler to track dependencies so
	// that we can make reloading work better when using includes:
	wcstombs(apath, wpath, MAX_PATH);
	{
		MigotoIncludeHandler include_handler(apath);
		hr = D3DCompile(srcData.data(), srcDataSize, apath, macros,
			G->recursive_include == -1 ? D3D_COMPILE_STANDARD_FILE_INCLUDE : &include_handler,
			"main", shaderModel, (UINT)compile_flags, 0, ppBytecode, &pErrorMsgs);
	}

	if (pErrorMsgs) {
		LPVOID errMsg = pErrorMsgs->GetBufferPointer();
		SIZE_T errSize = pErrorMsgs->GetBufferSize();
		LogInfo("--------------------------------------------- BEGIN ---------------------------------------------\n");
		LogOverlay(LOG_NOTICE, "%*s\n", errSize, errMsg);
		LogInfo("---------------------------------------------- END ----------------------------------------------\n");
		pErrorMsgs->Release();
	}

	if (FAILED(hr)) {
		LogOverlay(LOG_WARNING, "Error compiling custom shader\n");
		goto err;
	}

	if (G->CACHE_SHADERS) {
		FILE *fw;

		wfopen_ensuring_access(&fw, cache_path, L"wb");
		if (fw) {
			LogInfo("    Storing compiled shader to %S\n", cache_path);
			fwrite((*ppBytecode)->GetBufferPointer(), 1, (*ppBytecode)->GetBufferSize(), fw);
			fclose(fw);

			set_file_last_write_time(cache_path, &timestamp);
		} else
			LogInfo("    Error writing compiled shader to %S\n", cache_path);
	}

	return false;
err_close:
	CloseHandle(f);
err:
	return true;
}

void CustomShader::substantiate(ID3D11Device *mOrigDevice1)
{
	if (substantiated)
		return;
	substantiated = true;

	if (vs_bytecode) {
		mOrigDevice1->CreateVertexShader(vs_bytecode->GetBufferPointer(), vs_bytecode->GetBufferSize(), NULL, &vs);
		CleanupShaderMaps(vs);
		vs_bytecode->Release();
		vs_bytecode = NULL;
	}
	if (hs_bytecode) {
		mOrigDevice1->CreateHullShader(hs_bytecode->GetBufferPointer(), hs_bytecode->GetBufferSize(), NULL, &hs);
		CleanupShaderMaps(hs);
		hs_bytecode->Release();
		hs_bytecode = NULL;
	}
	if (ds_bytecode) {
		mOrigDevice1->CreateDomainShader(ds_bytecode->GetBufferPointer(), ds_bytecode->GetBufferSize(), NULL, &ds);
		CleanupShaderMaps(ds);
		ds_bytecode->Release();
		ds_bytecode = NULL;
	}
	if (gs_bytecode) {
		mOrigDevice1->CreateGeometryShader(gs_bytecode->GetBufferPointer(), gs_bytecode->GetBufferSize(), NULL, &gs);
		CleanupShaderMaps(gs);
		gs_bytecode->Release();
		gs_bytecode = NULL;
	}
	if (ps_bytecode) {
		mOrigDevice1->CreatePixelShader(ps_bytecode->GetBufferPointer(), ps_bytecode->GetBufferSize(), NULL, &ps);
		CleanupShaderMaps(ps);
		ps_bytecode->Release();
		ps_bytecode = NULL;
	}
	if (cs_bytecode) {
		mOrigDevice1->CreateComputeShader(cs_bytecode->GetBufferPointer(), cs_bytecode->GetBufferSize(), NULL, &cs);
		CleanupShaderMaps(cs);
		cs_bytecode->Release();
		cs_bytecode = NULL;
	}

	if (blend_override == 1) // 2 will merge the blend state at draw time
		mOrigDevice1->CreateBlendState(&blend_desc, &blend_state);

	if (depth_stencil_override == 1) // 2 will merge depth/stencil state at draw time
		mOrigDevice1->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state);

	if (rs_override == 1) // 2 will merge rasterizer state at draw time
		mOrigDevice1->CreateRasterizerState(&rs_desc, &rs_state);

	if (sampler_override == 1)
		mOrigDevice1->CreateSamplerState(&sampler_desc, &sampler_state);
}

// Similar to memcpy, but also takes a mask. Any bits in the mask that are set
// to 0 will be unchanged in the destination, while bits that are set to 1 will
// be copied from the source buffer.
static void memcpy_masked_merge(void *dest, void *src, void *mask, size_t n)
{
	char *c_dest = (char*)dest;
	char *c_src = (char*)src;
	char *c_mask = (char*)mask;
	size_t i;

	for (i = 0; i < n; i++)
		c_dest[i] = c_dest[i] & ~c_mask[i] | c_src[i] & c_mask[i];
}

void CustomShader::merge_blend_states(ID3D11BlendState *src_state, FLOAT src_blend_factor[4], UINT src_sample_mask, ID3D11Device *mOrigDevice1)
{
	D3D11_BLEND_DESC src_desc;
	int i;

	if (blend_override != 2)
		return;

	if (blend_state)
		blend_state->Release();
	blend_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.AlphaToCoverageEnable = FALSE;
		src_desc.IndependentBlendEnable = FALSE;
		for (i = 0; i < 8; i++) {
			src_desc.RenderTarget[i].BlendEnable = FALSE;
			src_desc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
			src_desc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
			src_desc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
			src_desc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
			src_desc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
			src_desc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			src_desc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
	}

	memcpy_masked_merge(&blend_desc, &src_desc, &blend_mask, sizeof(D3D11_BLEND_DESC));

	for (i = 0; i < 4; i++) {
		if (blend_factor_merge_mask[i])
			blend_factor[i] = src_blend_factor[i];
	}
	blend_sample_mask = blend_sample_mask & ~blend_sample_mask_merge_mask | src_sample_mask & blend_sample_mask_merge_mask;

	mOrigDevice1->CreateBlendState(&blend_desc, &blend_state);
}

void CustomShader::merge_depth_stencil_states(ID3D11DepthStencilState *src_state, UINT src_stencil_ref, ID3D11Device *mOrigDevice1)
{
	D3D11_DEPTH_STENCIL_DESC src_desc;

	if (depth_stencil_override != 2)
		return;

	if (depth_stencil_state)
		depth_stencil_state->Release();
	depth_stencil_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.DepthEnable = TRUE;
		src_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		src_desc.DepthFunc = D3D11_COMPARISON_LESS;
		src_desc.StencilEnable = FALSE;
		src_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		src_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
		src_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		src_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		src_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		src_desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		src_desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	}

	memcpy_masked_merge(&depth_stencil_desc, &src_desc, &depth_stencil_mask, sizeof(D3D11_DEPTH_STENCIL_DESC));
	stencil_ref = stencil_ref & ~stencil_ref_mask | src_stencil_ref & stencil_ref_mask;

	mOrigDevice1->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state);
}

void CustomShader::merge_rasterizer_states(ID3D11RasterizerState *src_state, ID3D11Device *mOrigDevice1)
{
	D3D11_RASTERIZER_DESC src_desc;

	if (rs_override != 2)
		return;

	if (rs_state)
		rs_state->Release();
	rs_state = NULL;

	if (src_state) {
		src_state->GetDesc(&src_desc);
	} else {
		// There is no state set, so DX will be using defaults. Set the
		// source description to the defaults so the merge will still
		// work as expected:
		src_desc.FillMode = D3D11_FILL_SOLID;
		src_desc.CullMode = D3D11_CULL_BACK;
		src_desc.FrontCounterClockwise = FALSE;
		src_desc.DepthBias = 0;
		src_desc.SlopeScaledDepthBias = 0.0f;
		src_desc.DepthBiasClamp = 0.0f;
		src_desc.DepthClipEnable = TRUE;
		src_desc.ScissorEnable = FALSE;
		src_desc.MultisampleEnable = FALSE;
		src_desc.AntialiasedLineEnable = FALSE;
	}

	memcpy_masked_merge(&rs_desc, &src_desc, &rs_mask, sizeof(D3D11_RASTERIZER_DESC));

	mOrigDevice1->CreateRasterizerState(&rs_desc, &rs_state);
}

struct saved_shader_inst
{
	ID3D11ClassInstance *instances[256];
	UINT num_instances;

	saved_shader_inst() :
		num_instances(0)
	{}

	~saved_shader_inst()
	{
		UINT i;

		for (i = 0; i < num_instances; i++) {
			if (instances[i])
				instances[i]->Release();
		}
	}
};

void RunCustomShaderCommand::run(CommandListState *state)
{
	ID3D11Device *mOrigDevice1 = state->mOrigDevice1;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11VertexShader *saved_vs = NULL;
	ID3D11HullShader *saved_hs = NULL;
	ID3D11DomainShader *saved_ds = NULL;
	ID3D11GeometryShader *saved_gs = NULL;
	ID3D11PixelShader *saved_ps = NULL;
	ID3D11ComputeShader *saved_cs = NULL;
	ID3D11BlendState *saved_blend = NULL;
	ID3D11DepthStencilState *saved_depth_stencil = NULL;
	ID3D11RasterizerState *saved_rs = NULL;
	UINT num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT saved_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	FLOAT saved_blend_factor[4];
	UINT saved_sample_mask;
	UINT saved_stencil_ref;
	bool saved_post;
	struct OMState om_state;
	UINT i;
	D3D11_PRIMITIVE_TOPOLOGY saved_topology;
	UINT num_sampler = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
	ID3D11SamplerState* saved_sampler_states[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];

	for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
	{
		saved_sampler_states[i] = nullptr;
	}

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (custom_shader->max_executions_per_frame) {
		if (custom_shader->frame_no != G->frame_no) {
			custom_shader->frame_no = G->frame_no;
			custom_shader->executions_this_frame = 1;
		} else if (custom_shader->executions_this_frame++ >= custom_shader->max_executions_per_frame) {
			COMMAND_LIST_LOG(state, "  max_executions_per_frame exceeded\n");
			Profiling::max_executions_per_frame_exceeded++;
			return;
		}
	}

	custom_shader->substantiate(mOrigDevice1);

	saved_shader_inst vs_inst, hs_inst, ds_inst, gs_inst, ps_inst, cs_inst;

	// Assign custom shaders first before running the command lists, and
	// restore them last. This is so that if someone was injecting a
	// sequence of pixel shaders that all shared a common vertex shader
	// we can avoid having to repeatedly save & restore the vertex shader
	// by calling the next shader in sequence from the command list after
	// the draw call.

	if (custom_shader->vs_override) {
		mOrigContext1->VSGetShader(&saved_vs, vs_inst.instances, &vs_inst.num_instances);
		mOrigContext1->VSSetShader(custom_shader->vs, NULL, 0);
	}
	if (custom_shader->hs_override) {
		mOrigContext1->HSGetShader(&saved_hs, hs_inst.instances, &hs_inst.num_instances);
		mOrigContext1->HSSetShader(custom_shader->hs, NULL, 0);
	}
	if (custom_shader->ds_override) {
		mOrigContext1->DSGetShader(&saved_ds, ds_inst.instances, &ds_inst.num_instances);
		mOrigContext1->DSSetShader(custom_shader->ds, NULL, 0);
	}
	if (custom_shader->gs_override) {
		mOrigContext1->GSGetShader(&saved_gs, gs_inst.instances, &gs_inst.num_instances);
		mOrigContext1->GSSetShader(custom_shader->gs, NULL, 0);
	}
	if (custom_shader->ps_override) {
		mOrigContext1->PSGetShader(&saved_ps, ps_inst.instances, &ps_inst.num_instances);
		mOrigContext1->PSSetShader(custom_shader->ps, NULL, 0);
	}
	if (custom_shader->cs_override) {
		mOrigContext1->CSGetShader(&saved_cs, cs_inst.instances, &cs_inst.num_instances);
		mOrigContext1->CSSetShader(custom_shader->cs, NULL, 0);
	}
	if (custom_shader->blend_override) {
		mOrigContext1->OMGetBlendState(&saved_blend, saved_blend_factor, &saved_sample_mask);
		custom_shader->merge_blend_states(saved_blend, saved_blend_factor, saved_sample_mask, mOrigDevice1);
		mOrigContext1->OMSetBlendState(custom_shader->blend_state, custom_shader->blend_factor, custom_shader->blend_sample_mask);
	}
	if (custom_shader->depth_stencil_override) {
		mOrigContext1->OMGetDepthStencilState(&saved_depth_stencil, &saved_stencil_ref);
		custom_shader->merge_depth_stencil_states(saved_depth_stencil, saved_stencil_ref, mOrigDevice1);
		mOrigContext1->OMSetDepthStencilState(custom_shader->depth_stencil_state, custom_shader->stencil_ref);
	}
	if (custom_shader->rs_override) {
		mOrigContext1->RSGetState(&saved_rs);
		custom_shader->merge_rasterizer_states(saved_rs, mOrigDevice1);
		mOrigContext1->RSSetState(custom_shader->rs_state);
	}
	if (custom_shader->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
		mOrigContext1->IAGetPrimitiveTopology(&saved_topology);
		mOrigContext1->IASetPrimitiveTopology(custom_shader->topology);
	}
	if (custom_shader->sampler_override) {
		mOrigContext1->PSGetSamplers(0, num_sampler, saved_sampler_states);
		mOrigContext1->PSSetSamplers(0, 1, &custom_shader->sampler_state); //just one slot for the moment TODO: allow more via *.ini file
	}
	// We save off the viewports unconditionally for now. We could
	// potentially skip this by flagging if a command list may alter them,
	// but that probably wouldn't buy us anything:
	mOrigContext1->RSGetViewports(&num_viewports, saved_viewports);
	// Likewise, save off all RTVs, UAVs and DSVs unconditionally:
	save_om_state(state->mOrigContext1, &om_state);

	// Run the command lists. This should generally include a draw or
	// dispatch call, or call out to another command list which does.
	// The reason for having a post command list is so that people can
	// write 'ps-t100 = ResourceFoo; post ps-t100 = null' and have it work.
	saved_post = state->post;
	state->post = false;
	_RunCommandList(&custom_shader->command_list, state);
	state->post = true;
	_RunCommandList(&custom_shader->post_command_list, state);
	state->post = saved_post;

	// Finally restore the original shaders
	if (custom_shader->vs_override)
		mOrigContext1->VSSetShader(saved_vs, vs_inst.instances, vs_inst.num_instances);
	if (custom_shader->hs_override)
		mOrigContext1->HSSetShader(saved_hs, hs_inst.instances, hs_inst.num_instances);
	if (custom_shader->ds_override)
		mOrigContext1->DSSetShader(saved_ds, ds_inst.instances, ds_inst.num_instances);
	if (custom_shader->gs_override)
		mOrigContext1->GSSetShader(saved_gs, gs_inst.instances, gs_inst.num_instances);
	if (custom_shader->ps_override)
		mOrigContext1->PSSetShader(saved_ps, ps_inst.instances, ps_inst.num_instances);
	if (custom_shader->cs_override)
		mOrigContext1->CSSetShader(saved_cs, cs_inst.instances, cs_inst.num_instances);
	if (custom_shader->blend_override)
		mOrigContext1->OMSetBlendState(saved_blend, saved_blend_factor, saved_sample_mask);
	if (custom_shader->depth_stencil_override)
		mOrigContext1->OMSetDepthStencilState(saved_depth_stencil, saved_stencil_ref);
	if (custom_shader->rs_override)
		mOrigContext1->RSSetState(saved_rs);
	if (custom_shader->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
		mOrigContext1->IASetPrimitiveTopology(saved_topology);
	if (custom_shader->sampler_override)
		mOrigContext1->PSSetSamplers(0, num_sampler, saved_sampler_states);

	mOrigContext1->RSSetViewports(num_viewports, saved_viewports);
	restore_om_state(mOrigContext1, &om_state);

	if (saved_vs)
		saved_vs->Release();
	if (saved_hs)
		saved_hs->Release();
	if (saved_ds)
		saved_ds->Release();
	if (saved_gs)
		saved_gs->Release();
	if (saved_ps)
		saved_ps->Release();
	if (saved_cs)
		saved_cs->Release();
	if (saved_blend)
		saved_blend->Release();
	if (saved_depth_stencil)
		saved_depth_stencil->Release();
	if (saved_rs)
		saved_rs->Release();

	for (i = 0; i < num_sampler; ++i) {
		if (saved_sampler_states[i])
			saved_sampler_states[i]->Release();
	}
}

bool RunCustomShaderCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	return (custom_shader->command_list.commands.empty() && custom_shader->post_command_list.commands.empty());
}

void RunExplicitCommandList::run(CommandListState *state)
{
	bool saved_post;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (run_pre_and_post_together) {
		saved_post = state->post;
		state->post = false;
		_RunCommandList(&command_list_section->command_list, state);
		state->post = true;
		_RunCommandList(&command_list_section->post_command_list, state);
		state->post = saved_post;
	} else if (state->post)
		_RunCommandList(&command_list_section->post_command_list, state);
	else
		_RunCommandList(&command_list_section->command_list, state);
}

bool RunExplicitCommandList::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	if (run_pre_and_post_together)
		return (command_list_section->command_list.commands.empty() && command_list_section->post_command_list.commands.empty());

	if (post)
		return command_list_section->post_command_list.commands.empty();
	return command_list_section->command_list.commands.empty();
}

std::shared_ptr<RunLinkedCommandList>
LinkCommandLists(CommandList *dst, CommandList *link, const wstring *ini_line)
{
	RunLinkedCommandList *operation = new RunLinkedCommandList(link);
	operation->ini_line = *ini_line;
	std::shared_ptr<RunLinkedCommandList> p(operation);
	dst->commands.push_back(p);
	return p;
}

void RunLinkedCommandList::run(CommandListState *state)
{
	_RunCommandList(link, state, false);
}

bool RunLinkedCommandList::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	return link->commands.empty();
}

static void ProcessParamRTSize(CommandListState *state)
{
	D3D11_RENDER_TARGET_VIEW_DESC view_desc;
	D3D11_TEXTURE2D_DESC res_desc;
	ID3D11RenderTargetView *view = NULL;
	ID3D11Resource *res = NULL;
	ID3D11Texture2D *tex = NULL;

	if (state->rt_width != -1)
		return;

	state->mOrigContext1->OMGetRenderTargets(1, &view, NULL);
	if (!view)
		return;

	view->GetDesc(&view_desc);

	if (view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
	    view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)
		goto out_release_view;

	view->GetResource(&res);
	if (!res)
		goto out_release_view;

	tex = (ID3D11Texture2D *)res;
	tex->GetDesc(&res_desc);

	state->rt_width = (float)res_desc.Width;
	state->rt_height = (float)res_desc.Height;

	tex->Release();
out_release_view:
	view->Release();
}

static void UpdateScissorInfo(CommandListState *state)
{
	UINT num = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

	if (state->scissor_valid)
		return;

	state->mOrigContext1->RSGetScissorRects(&num, state->scissor_rects);

	state->scissor_valid = true;
}

float CommandListOperand::process_texture_filter(CommandListState *state)
{
	TextureOverrideMatches matches;
	TextureOverrideMatches::reverse_iterator rit;
	bool resource_found;

	texture_filter_target.FindTextureOverrides(state, &resource_found, &matches);

	// If there is no resource bound we want to return a special value that
	// is distinct from simply not finding a texture override section. For
	// backwards compatibility we use negative zero -0.0, because any
	// existing fixes that test for zero/non-zero to check if a matching
	// [TextureOverride] is present would expect an unbound texture to
	// never have a hash and therefore be equal to 0, and -0.0 *is* equal
	// to +0, so these will continue to work. To explicitly test for an
	// unassigned resource, use this HLSL to reinterpret the values as
	// integers and check the sign bit:
	//
	// if (asint(IniParams[0].x) == asint(-0.0)) { ... }
	//
	if (!resource_found)
		return -0.0;

	// A resource was bound, but no matching texture override was found:
	if (matches.empty())
		return 0;

	// If there are multiple matches, we want the filter_index with the
	// highest priority, which will be the last in the list that has a
	// filter index. In the future we may also want a namespaced version of
	// this (and checktextureoverride) to limit the check to sections
	// appearing in the same namespace or with a given prefix (but we don't
	// want to do string processing on the namespace here - the candidates
	// should already be narrowed down during ini parsing):
	for (rit = matches.rbegin(); rit != matches.rend(); rit++) {
		if ((*rit)->filter_index != FLT_MAX)
			return (*rit)->filter_index;
	}

	// No match had a filter_index, but there was at least one match:
	return 1.0;
}

float CommandListOperand::process_shader_filter(CommandListState *state)
{
	HackerContext *mHackerContext = state->mHackerContext;
	ID3D11DeviceChild *shader = NULL;

	switch (shader_filter_target) {
		case L'v':
			shader = mHackerContext->mCurrentVertexShaderHandle;
			break;
		case L'h':
			shader = mHackerContext->mCurrentHullShaderHandle;
			break;
		case L'd':
			shader = mHackerContext->mCurrentDomainShaderHandle;
			break;
		case L'g':
			shader = mHackerContext->mCurrentGeometryShaderHandle;
			break;
		case L'p':
			shader = mHackerContext->mCurrentPixelShaderHandle;
			break;
		case L'c':
			shader = mHackerContext->mCurrentComputeShaderHandle;
			break;
		default:
			LogOverlay(LOG_DIRE, "BUG: Unknown shader filter type: \"%C\"\n", shader_filter_target);
			break;
	}

	// Negative zero means no shader bound:
	if (!shader)
		return -0.0;

	ShaderMap::iterator shader_it = lookup_shader_hash(shader);

	if (shader_it == G->mShaders.end())
		return 0.0;

	// Positive zero means shader bound with no ShaderOverride
	ShaderOverrideMap::iterator override = lookup_shaderoverride(shader_it->second);
	if (override == G->mShaderOverrideMap.end())
		return 0.0;

	if (override->second.filter_index != FLT_MAX)
		return override->second.filter_index;

	// Matched ShaderOverride / ShaderRegex, but no filter_index:
	return 1.0;
}

void CommandList::clear()
{
	commands.clear();
	static_vars.clear();
}

CommandListState::CommandListState() :
	mHackerDevice(NULL),
	mHackerContext(NULL),
	mOrigDevice1(NULL),
	mOrigContext1(NULL),
	rt_width(-1),
	rt_height(-1),
	call_info(NULL),
	this_target(NULL),
	resource(NULL),
	view(NULL),
	post(false),
	update_params(false),
	cursor_mask_tex(NULL),
	cursor_mask_view(NULL),
	cursor_color_tex(NULL),
	cursor_color_view(NULL),
	recursion(0),
	extra_indent(0),
	aborted(false),
	scissor_valid(false)
{
	memset(&cursor_info, 0, sizeof(CURSORINFO));
	memset(&cursor_info_ex, 0, sizeof(ICONINFO));
	memset(&window_rect, 0, sizeof(RECT));
}

CommandListState::~CommandListState()
{
	if (cursor_info_ex.hbmMask)
		DeleteObject(cursor_info_ex.hbmMask);
	if (cursor_info_ex.hbmColor)
		DeleteObject(cursor_info_ex.hbmColor);
	if (cursor_mask_view)
		cursor_mask_view->Release();
	if (cursor_mask_tex)
		cursor_mask_tex->Release();
	if (cursor_color_view)
		cursor_color_view->Release();
	if (cursor_color_tex)
		cursor_color_tex->Release();
}

static void UpdateWindowInfo(CommandListState *state)
{
	if (state->window_rect.right)
		return;

	if (G->hWnd)
		CursorUpscalingBypass_GetClientRect(G->hWnd, &state->window_rect);
	else
		LogDebug("UpdateWindowInfo: No hWnd\n");
}

static void UpdateCursorInfo(CommandListState *state)
{
	if (state->cursor_info.cbSize)
		return;

	state->cursor_info.cbSize = sizeof(CURSORINFO);
	CursorUpscalingBypass_GetCursorInfo(&state->cursor_info);
	memcpy(&state->cursor_window_coords, &state->cursor_info.ptScreenPos, sizeof(POINT));

	if (G->hWnd)
		CursorUpscalingBypass_ScreenToClient(G->hWnd, &state->cursor_window_coords);
	else
		LogDebug("UpdateCursorInfo: No hWnd\n");
}

static void UpdateCursorInfoEx(CommandListState *state)
{
	if (state->cursor_info_ex.hbmMask)
		return;

	UpdateCursorInfo(state);

	GetIconInfo(state->cursor_info.hCursor, &state->cursor_info_ex);
}

// Uses an undocumented Windows API to get info about animated cursors and
// calculate the current frame based on the global tick count
// https://stackoverflow.com/questions/6969801/how-do-i-determine-if-the-current-mouse-cursor-is-animated
static unsigned GetCursorFrame(HCURSOR cursor)
{
	typedef HCURSOR(WINAPI* GET_CURSOR_FRAME_INFO)(HCURSOR, LPCWSTR, DWORD, DWORD*, DWORD*);
	static GET_CURSOR_FRAME_INFO fnGetCursorFrameInfo = NULL;
	HMODULE libUser32 = NULL;
	DWORD period = 6, frames = 1;

	if (!fnGetCursorFrameInfo) {
		libUser32 = LoadLibraryA("user32.dll");
		if (!libUser32)
			return 0;

		fnGetCursorFrameInfo = (GET_CURSOR_FRAME_INFO)GetProcAddress(libUser32, "GetCursorFrameInfo");
		if (!fnGetCursorFrameInfo)
			return 0;
	}

	fnGetCursorFrameInfo(cursor, L"", 0, &period, &frames);

	// Avoid divide by zero if not an animated cursor:
	if (!period || !frames)
		return 0;

	// period is a multiple of 1/60 seconds. We should really use the ms
	// since this cursor was most recently displayed, but the global tick
	// count works well enough and means we have less state to track:
	return (GetTickCount() * 6) / (period * 100) % frames;
}

static void _CreateTextureFromBitmap(HDC dc, BITMAP *bitmap_obj,
		HBITMAP hbitmap, CommandListState *state,
		ID3D11Texture2D **tex, ID3D11ShaderResourceView **view)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC rv_desc;
	BITMAPINFOHEADER bmp_info;
	D3D11_SUBRESOURCE_DATA data;
	D3D11_TEXTURE2D_DESC desc;
	HRESULT hr;

	bmp_info.biSize = sizeof(BITMAPINFOHEADER);
	bmp_info.biWidth = bitmap_obj->bmWidth;
	bmp_info.biHeight = bitmap_obj->bmHeight;
	// Requesting 32bpp here to simplify the conversion process - the
	// R1_UNORM format can't be used for the 1bpp mask because that format
	// has a special purpose, and requesting 8 or 16bpp would require an
	// array of RGBQUADs after the BITMAPINFO structure for the pallette
	// that I don't want to deal with, and there is no DXGI_FORMAT for
	// 24bpp... 32bpp should work for both the mask and palette:
	bmp_info.biBitCount = 32;
	bmp_info.biPlanes = 1;
	bmp_info.biCompression = BI_RGB;
	// Pretty sure these are ignored / output only in GetDIBits:
	bmp_info.biSizeImage = 0;
	bmp_info.biXPelsPerMeter = 0;
	bmp_info.biYPelsPerMeter = 0;
	bmp_info.biClrUsed = 0;
	bmp_info.biClrImportant = 0;

	// This padding came from an example on MSDN, but I can't find
	// the documentation that indicates exactly what it is supposed
	// to be. Since we're using 32bpp, this shouldn't matter anyway:
	data.SysMemPitch = ((bitmap_obj->bmWidth * bmp_info.biBitCount + 31) / 32) * 4;

	data.pSysMem = new char[data.SysMemPitch * bitmap_obj->bmHeight];

	if (!GetDIBits(dc, hbitmap, 0, bmp_info.biHeight,
			(LPVOID)data.pSysMem, (BITMAPINFO*)&bmp_info, DIB_RGB_COLORS)) {
		LogInfo("Software Mouse: GetDIBits() failed\n");
		goto err_free;
	}

	desc.Width = bitmap_obj->bmWidth;
	desc.Height = bitmap_obj->bmHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	// FIXME: Use DXGI_FORMAT_B8G8R8X8_UNORM_SRGB if no alpha channel (there is no API to check)
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	LockResourceCreationMode();
	hr = state->mOrigDevice1->CreateTexture2D(&desc, &data, tex);
	UnlockResourceCreationMode();
	if (FAILED(hr)) {
		LogInfo("Software Mouse: CreateTexture2D Failed: 0x%x\n", hr);
		goto err_free;
	}

	rv_desc.Format = desc.Format;
	rv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	rv_desc.Texture2D.MostDetailedMip = 0;
	rv_desc.Texture2D.MipLevels = 1;

	hr = state->mOrigDevice1->CreateShaderResourceView(*tex, &rv_desc, view);
	if (FAILED(hr)) {
		LogInfo("Software Mouse: CreateShaderResourceView Failed: 0x%x\n", hr);
		goto err_release_tex;
	}

	delete [] data.pSysMem;

	return;
err_release_tex:
	(*tex)->Release();
	*tex = NULL;
err_free:
	delete [] data.pSysMem;
}

static void CreateTextureFromBitmap(HDC dc, HBITMAP hbitmap, CommandListState *state,
		ID3D11Texture2D **tex, ID3D11ShaderResourceView **view)
{
	BITMAP bitmap_obj;

	if (!GetObject(hbitmap, sizeof(BITMAP), &bitmap_obj)) {
		LogInfo("Software Mouse: GetObject() failed\n");
		return;
	}

	_CreateTextureFromBitmap(dc, &bitmap_obj, hbitmap, state, tex, view);
}

static void CreateTextureFromAnimatedCursor(
		HDC dc,
		HCURSOR cursor,
		UINT flags,
		HBITMAP static_bitmap,
		CommandListState *state,
		ID3D11Texture2D **tex,
		ID3D11ShaderResourceView **view
		)
{
	BITMAP bitmap_obj;
	HDC dc_mem;
	HBITMAP ani_bitmap;
	unsigned frame;

	if (!GetObject(static_bitmap, sizeof(BITMAP), &bitmap_obj)) {
		LogInfo("Software Mouse: GetObject() failed\n");
		return;
	}

	dc_mem = CreateCompatibleDC(dc);
	if (!dc_mem) {
		LogInfo("Software Mouse: CreateCompatibleDC() failed\n");
		return;
	}

	ani_bitmap = CreateCompatibleBitmap(dc, bitmap_obj.bmWidth, bitmap_obj.bmHeight);
	if (!ani_bitmap) {
		LogInfo("Software Mouse: CreateCompatibleBitmap() failed\n");
		goto out_delete_mem_dc;
	}

	frame = GetCursorFrame(cursor);

	// To get a frame from an animated cursor we have to use DrawIconEx to
	// draw it to another bitmap, then we can create a texture from that
	// bitmap:
	SelectObject(dc_mem, ani_bitmap);
	if (!DrawIconEx(dc_mem, 0, 0, cursor, bitmap_obj.bmWidth, bitmap_obj.bmHeight, frame, NULL, flags)) {
		LogInfo("Software Mouse: DrawIconEx failed\n");
		// Fall back to getting the first frame from the static_bitmap we already have:
		_CreateTextureFromBitmap(dc, &bitmap_obj, static_bitmap, state, tex, view);
		goto out_delete_ani_bitmap;
	}

	_CreateTextureFromBitmap(dc, &bitmap_obj, ani_bitmap, state, tex, view);

out_delete_ani_bitmap:
	DeleteObject(ani_bitmap);
out_delete_mem_dc:
	DeleteDC(dc_mem);
}

static void UpdateCursorResources(CommandListState *state)
{
	HDC dc;
	Profiling::State profiling_state;

	if (state->cursor_mask_tex || state->cursor_color_tex)
		return;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	UpdateCursorInfoEx(state);

	// XXX: Should maybe be the device context for the window?
	dc = GetDC(NULL);
	if (!dc) {
		LogInfo("Software Mouse: GetDC() failed\n");
		return;
	}

	if (state->cursor_info_ex.hbmColor) {
		// Colour cursor, which may or may not be animated, but the
		// animated routine will work either way:
		CreateTextureFromAnimatedCursor(
				dc,
				state->cursor_info.hCursor,
				DI_IMAGE,
				state->cursor_info_ex.hbmColor,
				state,
				&state->cursor_color_tex,
				&state->cursor_color_view);

		if (state->cursor_info_ex.hbmMask) {
			// Since it's a colour cursor the mask bitmap will be
			// the regular height, which will work with the
			// animated routine:
			CreateTextureFromAnimatedCursor(
					dc,
					state->cursor_info.hCursor,
					DI_MASK,
					state->cursor_info_ex.hbmMask,
					state,
					&state->cursor_mask_tex,
					&state->cursor_mask_view);
		}
	} else if (state->cursor_info_ex.hbmMask) {
		// Black and white cursor, which means the hbmMask bitmap is
		// double height and won't work with the animated cursor
		// routines, so just turn the bitmap into a texture directly:
		CreateTextureFromBitmap(
				dc,
				state->cursor_info_ex.hbmMask,
				state,
				&state->cursor_mask_tex,
				&state->cursor_mask_view);
	}

	ReleaseDC(NULL, dc);

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::cursor_overhead);
}

static bool sli_enabled(HackerDevice *device)
{
	NV_GET_CURRENT_SLI_STATE sli_state;
	sli_state.version = NV_GET_CURRENT_SLI_STATE_VER;
	NvAPI_Status status;

	status = Profiling::NvAPI_D3D_GetCurrentSLIState(device->GetPossiblyHookedOrigDevice1(), &sli_state);
	if (status != NVAPI_OK) {
		LogInfo("Unable to retrieve SLI state from nvapi\n");
		return false;
	}

	return sli_state.maxNumAFRGroups > 1;
}

float CommandListOperand::evaluate(CommandListState *state, HackerDevice *device)
{
	NvU8 stereo = false;
	float fret;

	if (state)
		device = state->mHackerDevice;
	else if (!device) {
		LogOverlay(LOG_DIRE, "BUG: CommandListOperand::evaluate called with neither state nor device\n");
		return 0;
	}

	// XXX: If updating this list, be sure to also update
	// XXX: operand_allowed_in_context()
	switch (type) {
		case ParamOverrideType::VALUE:
			return val;
		case ParamOverrideType::INI_PARAM:
			return G->iniParams[param_idx].*param_component;
		case ParamOverrideType::VARIABLE:
			return *var_ftarget;
		case ParamOverrideType::RES_WIDTH:
			return (float)G->mResolutionInfo.width;
		case ParamOverrideType::RES_HEIGHT:
			return (float)G->mResolutionInfo.height;
		case ParamOverrideType::TIME:
			return (float)(GetTickCount() - G->ticks_at_launch) / 1000.0f;
		case ParamOverrideType::RAW_SEPARATION:
			// We could use cached values of these (nvapi is known
			// to become a bottleneck with too many calls / frame),
			// but they need to be up to date, taking into account
			// any changes made via the command list already this
			// frame (this is used for snapshots and getting the
			// current convergence regardless of whether an
			// asynchronous transfer from the GPU has or has not
			// completed) - StereoParams is currently unsuitable
			// for this as it is only updated once / frame... We
			// could change it so that StereoParams is always up to
			// date - it would differ from the historical
			// behaviour, but I doubt it would break anything.
			// Otherwise we could have a separate cache. Whatever -
			// this is rarely used, so let's just go with this for
			// now and worry about optimisations only if it proves
			// to be a bottleneck in practice:
			Profiling::NvAPI_Stereo_GetSeparation(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::CONVERGENCE:
			Profiling::NvAPI_Stereo_GetConvergence(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::EYE_SEPARATION:
			Profiling::NvAPI_Stereo_GetEyeSeparation(device->mStereoHandle, &fret);
			return fret;
		case ParamOverrideType::STEREO_ACTIVE:
			Profiling::NvAPI_Stereo_IsActivated(device->mStereoHandle, &stereo);
			return !!stereo;
		case ParamOverrideType::STEREO_AVAILABLE:
			Profiling::NvAPI_Stereo_IsEnabled(&stereo);
			return !!stereo;
		case ParamOverrideType::SLI:
			return sli_enabled(device);
		case ParamOverrideType::HUNTING:
			return (float)G->hunting;
		case ParamOverrideType::FRAME_ANALYSIS:
			return G->analyse_frame;
		case ParamOverrideType::EFFECTIVE_DPI:
			return get_effective_dpi();
		// XXX: If updating this list, be sure to also update
		// XXX: operand_allowed_in_context()
	}

	if (!state) {
		// FIXME: Some of these only use the state object for cache,
		// and could still be evaluated if we forgo the cache
		LogOverlay(LOG_WARNING, "BUG: Operand type %i cannot be evaluated outside of a command list\n", type);
		return 0;
	}

	switch (type) {
		case ParamOverrideType::RT_WIDTH:
			ProcessParamRTSize(state);
			return state->rt_width;
		case ParamOverrideType::RT_HEIGHT:
			ProcessParamRTSize(state);
			return state->rt_height;
		case ParamOverrideType::WINDOW_WIDTH:
			UpdateWindowInfo(state);
			return (float)state->window_rect.right;
		case ParamOverrideType::WINDOW_HEIGHT:
			UpdateWindowInfo(state);
			return (float)state->window_rect.bottom;
		case ParamOverrideType::TEXTURE:
			return process_texture_filter(state);
		case ParamOverrideType::SHADER:
			return process_shader_filter(state);
		case ParamOverrideType::VERTEX_COUNT:
			if (state->call_info)
				return (float)state->call_info->VertexCount;
			return 0;
		case ParamOverrideType::INDEX_COUNT:
			if (state->call_info)
				return (float)state->call_info->IndexCount;
			return 0;
		case ParamOverrideType::INSTANCE_COUNT:
			if (state->call_info)
				return (float)state->call_info->InstanceCount;
			return 0;
		case ParamOverrideType::FIRST_VERTEX:
			if (state->call_info)
				return (float)state->call_info->FirstVertex;
			return 0;
		case ParamOverrideType::FIRST_INDEX:
			if (state->call_info)
				return (float)state->call_info->FirstIndex;
			return 0;
		case ParamOverrideType::FIRST_INSTANCE:
			if (state->call_info)
				return (float)state->call_info->FirstInstance;
			return 0;
		case ParamOverrideType::THREAD_GROUP_COUNT_X:
			if (state->call_info)
				return (float)state->call_info->ThreadGroupCountX;
			return 0;
		case ParamOverrideType::THREAD_GROUP_COUNT_Y:
			if (state->call_info)
				return (float)state->call_info->ThreadGroupCountY;
			return 0;
		case ParamOverrideType::THREAD_GROUP_COUNT_Z:
			if (state->call_info)
				return (float)state->call_info->ThreadGroupCountZ;
			return 0;
		case ParamOverrideType::INDIRECT_OFFSET:
			if (state->call_info)
				return (float)state->call_info->args_offset;
			return 0;
		case ParamOverrideType::DRAW_TYPE:
			if (state->call_info)
				return (float)state->call_info->type;
			return 0;
		case ParamOverrideType::CURSOR_VISIBLE:
			UpdateCursorInfo(state);
			return !!(state->cursor_info.flags & CURSOR_SHOWING);
		case ParamOverrideType::CURSOR_SCREEN_X:
			UpdateCursorInfo(state);
			return (float)state->cursor_info.ptScreenPos.x;
		case ParamOverrideType::CURSOR_SCREEN_Y:
			UpdateCursorInfo(state);
			return (float)state->cursor_info.ptScreenPos.y;
		case ParamOverrideType::CURSOR_WINDOW_X:
			UpdateCursorInfo(state);
			return (float)state->cursor_window_coords.x;
		case ParamOverrideType::CURSOR_WINDOW_Y:
			UpdateCursorInfo(state);
			return (float)state->cursor_window_coords.y;
		case ParamOverrideType::CURSOR_X:
			UpdateCursorInfo(state);
			UpdateWindowInfo(state);
			return (float)state->cursor_window_coords.x / (float)state->window_rect.right;
		case ParamOverrideType::CURSOR_Y:
			UpdateCursorInfo(state);
			UpdateWindowInfo(state);
			return (float)state->cursor_window_coords.y / (float)state->window_rect.bottom;
		case ParamOverrideType::CURSOR_HOTSPOT_X:
			UpdateCursorInfoEx(state);
			return (float)state->cursor_info_ex.xHotspot;
		case ParamOverrideType::CURSOR_HOTSPOT_Y:
			UpdateCursorInfoEx(state);
			return (float)state->cursor_info_ex.yHotspot;
		case ParamOverrideType::SCISSOR_LEFT:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].left;
		case ParamOverrideType::SCISSOR_TOP:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].top;
		case ParamOverrideType::SCISSOR_RIGHT:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].right;
		case ParamOverrideType::SCISSOR_BOTTOM:
			UpdateScissorInfo(state);
			return (float)state->scissor_rects[scissor].bottom;
	}

	LogOverlay(LOG_DIRE, "BUG: Unhandled operand type %i\n", type);
	return 0;
}

bool CommandListOperand::static_evaluate(float *ret, HackerDevice *device)
{
	NvU8 stereo = false;

	switch (type) {
		case ParamOverrideType::VALUE:
			*ret = val;
			return true;
		case ParamOverrideType::RAW_SEPARATION:
		case ParamOverrideType::CONVERGENCE:
		case ParamOverrideType::EYE_SEPARATION:
		case ParamOverrideType::STEREO_ACTIVE:
			NvAPIOverride();
			Profiling::NvAPI_Stereo_IsEnabled(&stereo);
			if (!stereo) {
				*ret = 0.0;
				return true;
			}
			break;
		case ParamOverrideType::STEREO_AVAILABLE:
			Profiling::NvAPI_Stereo_IsEnabled(&stereo);
			*ret = stereo;
			return true;
		case ParamOverrideType::SLI:
			if (device) {
				*ret = sli_enabled(device);
				return true;
			}
			break;
		case ParamOverrideType::HUNTING:
		case ParamOverrideType::FRAME_ANALYSIS:
			if (G->hunting == HUNTING_MODE_DISABLED) {
				*ret = 0;
				return true;
			}
			break;
	}

	return false;
}

bool CommandListOperand::optimise(HackerDevice *device, std::shared_ptr<CommandListEvaluatable> *replacement)
{
	if (type == ParamOverrideType::VALUE)
		return false;

	if (!static_evaluate(&val, device))
		return false;

	LogInfo("Statically evaluated %S as %f\n",
		lookup_enum_name(ParamOverrideTypeNames, type), val);

	type = ParamOverrideType::VALUE;
	return true;
}

static const wchar_t *operator_tokens[] = {
	// Three character tokens first:
	L"===", L"!==",
	// Two character tokens next:
	L"==", L"!=", L"//", L"<=", L">=", L"&&", L"||", L"**",
	// Single character tokens last:
	L"(", L")", L"!", L"*", L"/", L"%", L"+", L"-", L"<", L">",
};

class CommandListSyntaxError: public exception
{
public:
	wstring msg;
	size_t pos;

	CommandListSyntaxError(wstring msg, size_t pos) :
		msg(msg), pos(pos)
	{}
};

static void tokenise(const wstring *expression, CommandListSyntaxTree *tree, const wstring *ini_namespace, CommandListScope *scope)
{
	wstring remain = *expression;
	ResourceCopyTarget texture_filter_target;
	shared_ptr<CommandListOperand> operand;
	wstring token;
	size_t pos = 0;
	int ipos = 0;
	size_t friendly_pos = 0;
	float fval;
	int ret;
	int i;
	bool last_was_operand = false;

	LogDebug("    Tokenising \"%S\"\n", expression->c_str());

	while (true) {
next_token:
		// Skip whitespace:
		pos = remain.find_first_not_of(L" \t", pos);
		if (pos == wstring::npos)
			return;
		remain = remain.substr(pos);
		friendly_pos += pos;

		// Operators:
		for (i = 0; i < ARRAYSIZE(operator_tokens); i++) {
			if (!remain.compare(0, wcslen(operator_tokens[i]), operator_tokens[i])) {
				pos = wcslen(operator_tokens[i]);
				tree->tokens.emplace_back(make_shared<CommandListOperatorToken>(friendly_pos, remain.substr(0, pos)));
				LogDebug("      Operator: \"%S\"\n", tree->tokens.back()->token.c_str());
				last_was_operand = false;
				goto next_token; // continue would continue wrong loop
			}
		}

		// Texture Filtering / Resource Slots:
		// - Many of these slots include a hyphen character, which
		//   conflicts with the subtraction/negation operators,
		//   potentially making something like "x = ps-t0" ambiguous as
		//   to whether it is referring to pixel shader texture slot 0,
		//   or subtracting "t0" from "ps", but in practice this should
		//   be generally be fine since we don't have anything called
		//   "ps", "t0" or similar, and if we did simply adding
		//   whitespace around the subtraction would disambiguate it.
		// - The characters we check for here preclude some arbitrary
		//   custom Resource names, including namespaced resources, but
		//   that's ok since this is only for texture filtering, which
		//   doesn't work if custom resources are checked. If we need
		//   to match these for some other reason, we could add \ and .
		//   to this list, which will cover most namespaced resources.
		pos = remain.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_-0123456789");
		if (pos) {
			token = remain.substr(0, pos);
			ret = texture_filter_target.ParseTarget(token.c_str(), true, ini_namespace);
			if (ret) {
				operand = make_shared<CommandListOperand>(friendly_pos, token);
				if (operand->parse(&token, ini_namespace, scope)) {
					tree->tokens.emplace_back(std::move(operand));
					LogDebug("      Resource Slot: \"%S\"\n", tree->tokens.back()->token.c_str());
					if (last_was_operand)
						throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
					last_was_operand = true;
					continue;
				} else {
					LogOverlay(LOG_DIRE, "BUG: Token parsed as resource slot, but not as operand: \"%S\"\n", token.c_str());
					throw CommandListSyntaxError(L"BUG", friendly_pos);
				}
			}
		}

		// Identifiers:
		// - Parse this before floats to make sure that the special
		//   cases "inf" and "nan" are identifiers by themselves, not
		//   the start of some other identifier. Only applies to
		//   vs2015+ as older toolchains lack parsing for these.
		// - Identifiers cannot start with a number
		// - Variable identifiers start with a $, and these may be
		//   namespaced, so we allow backslash and . as well
		//   TODO: Be more specific with namespaces to allow exactly
		//   the set of actual namespaces. Would allow for namespaces
		//   to have spaces or other unusual characters while freeing
		//   up . \ and $ for potential use as operators in the future.
		if (remain[0] < '0' || remain[0] > '9') {
			pos = remain.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_0123456789$\\.");
			if (pos) {
				token = remain.substr(0, pos);
				operand = make_shared<CommandListOperand>(friendly_pos, token);
				if (operand->parse(&token, ini_namespace, scope)) {
					tree->tokens.emplace_back(std::move(operand));
					LogDebug("      Identifier: \"%S\"\n", tree->tokens.back()->token.c_str());
					if (last_was_operand)
						throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
					last_was_operand = true;
					continue;
				}
				throw CommandListSyntaxError(L"Unrecognised identifier: " + token, friendly_pos);
			}
		}

		// Floats:
		// - Must tokenise subtraction operation first
		//   - Static optimisation will merge unary negation
		// - Identifier match will catch "nan" and "inf" special cases
		//   if the toolchain supports them
		ret = swscanf_s(remain.c_str(), L"%f%n", &fval, &ipos);
		if (ret != 0 && ret != EOF) {
			// VS2013 Issue: size_t z/I modifiers do not work with %n
			// We could make pos an int and cast it everywhere it is used
			// as a size_t, but this way highlights the toolchain issue.
			pos = ipos;

			token = remain.substr(0, ipos);
			operand = make_shared<CommandListOperand>(friendly_pos, token);
			if (operand->parse(&token, ini_namespace, scope)) {
				tree->tokens.emplace_back(std::move(operand));
				LogDebug("      Float: \"%S\"\n", tree->tokens.back()->token.c_str());
				if (last_was_operand)
					throw CommandListSyntaxError(L"Unexpected identifier", friendly_pos);
				last_was_operand = true;
				continue;
			} else {
				LogOverlay(LOG_DIRE, "BUG: Token parsed as float, but not as operand: \"%S\"\n", token.c_str());
				throw CommandListSyntaxError(L"BUG", friendly_pos);
			}
		}

		throw CommandListSyntaxError(L"Parse error", friendly_pos);
	}
}

static void group_parenthesis(CommandListSyntaxTree *tree)
{
	CommandListSyntaxTree::Tokens::iterator i;
	CommandListSyntaxTree::Tokens::reverse_iterator rit;
	CommandListOperatorToken *rbracket, *lbracket;
	std::shared_ptr<CommandListSyntaxTree> inner;

	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		rbracket = dynamic_cast<CommandListOperatorToken*>(i->get());
		if (rbracket && !rbracket->token.compare(L")")) {
			for (rit = std::reverse_iterator<CommandListSyntaxTree::Tokens::iterator>(i); rit != tree->tokens.rend(); rit++) {
				lbracket = dynamic_cast<CommandListOperatorToken*>(rit->get());
				if (lbracket && !lbracket->token.compare(L"(")) {
					inner = std::make_shared<CommandListSyntaxTree>(lbracket->token_pos);
					// XXX: Double check bounds are right:
					inner->tokens.assign(rit.base(), i);
					i = tree->tokens.erase(rit.base() - 1, i + 1);
					i = tree->tokens.insert(i, std::move(inner));
					goto continue_rbracket_search; // continue would continue wrong loop
				}
			}
			throw CommandListSyntaxError(L"Unmatched )", rbracket->token_pos);
		}
	continue_rbracket_search: false;
	}

	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		lbracket = dynamic_cast<CommandListOperatorToken*>(i->get());
		if (lbracket && !lbracket->token.compare(L"("))
			throw CommandListSyntaxError(L"Unmatched (", lbracket->token_pos);
	}
}

// Expression operator definitions:
#define DEFINE_OPERATOR(name, operator_pattern, fn) \
class name##T : public CommandListOperator { \
public: \
	name##T( \
			std::shared_ptr<CommandListToken> lhs, \
			CommandListOperatorToken &t, \
			std::shared_ptr<CommandListToken> rhs \
		) : CommandListOperator(lhs, t, rhs) \
	{} \
	static const wchar_t* pattern() { return L##operator_pattern; } \
	float evaluate(float lhs, float rhs) override { return (fn); } \
}; \
static CommandListOperatorFactory<name##T> name;

// Highest level of precedence, allows for negative numbers
DEFINE_OPERATOR(unary_not_operator,     "!",  (!rhs));
DEFINE_OPERATOR(unary_plus_operator,    "+",  (+rhs));
DEFINE_OPERATOR(unary_negate_operator,  "-",  (-rhs));

// High level of precedence, right-associative. Lower than unary operators, so
// that 4**-2 works for square root
DEFINE_OPERATOR(exponent_operator,      "**", (pow(lhs, rhs)));

DEFINE_OPERATOR(multiplication_operator,"*",  (lhs * rhs));
DEFINE_OPERATOR(division_operator,      "/",  (lhs / rhs));
DEFINE_OPERATOR(floor_division_operator,"//", (floor(lhs / rhs)));
DEFINE_OPERATOR(modulus_operator,       "%",  (fmod(lhs, rhs)));

DEFINE_OPERATOR(addition_operator,      "+",  (lhs + rhs));
DEFINE_OPERATOR(subtraction_operator,   "-",  (lhs - rhs));

DEFINE_OPERATOR(less_operator,          "<",  (lhs < rhs));
DEFINE_OPERATOR(less_equal_operator,    "<=", (lhs <= rhs));
DEFINE_OPERATOR(greater_operator,       ">",  (lhs > rhs));
DEFINE_OPERATOR(greater_equal_operator, ">=", (lhs >= rhs));

// The triple equals operator tests for binary equivalence - in particular,
// this allows us to test for negative zero, used in texture filtering to
// signify that nothing is bound to a given slot. Negative zero cannot be
// tested for using the regular equals operator, since -0.0 == +0.0. This
// operator could also test for specific cases of NAN (though, without the
// vs2015 toolchain "nan" won't parse as such).
DEFINE_OPERATOR(equality_operator,      "==", (lhs == rhs));
DEFINE_OPERATOR(inequality_operator,    "!=", (lhs != rhs));
DEFINE_OPERATOR(identical_operator,     "===",(*(uint32_t*)&lhs == *(uint32_t*)&rhs));
DEFINE_OPERATOR(not_identical_operator, "!==",(*(uint32_t*)&lhs != *(uint32_t*)&rhs));

DEFINE_OPERATOR(and_operator,           "&&", (lhs && rhs));

DEFINE_OPERATOR(or_operator,            "||", (lhs || rhs));

// TODO: Ternary if operator

static CommandListOperatorFactoryBase *unary_operators[] = {
	&unary_not_operator,
	&unary_negate_operator,
	&unary_plus_operator,
};
static CommandListOperatorFactoryBase *exponent_operators[] = {
	&exponent_operator,
};
static CommandListOperatorFactoryBase *multi_division_operators[] = {
	&multiplication_operator,
	&division_operator,
	&floor_division_operator,
	&modulus_operator,
};
static CommandListOperatorFactoryBase *add_subtract_operators[] = {
	&addition_operator,
	&subtraction_operator,
};
static CommandListOperatorFactoryBase *relational_operators[] = {
	&less_operator,
	&less_equal_operator,
	&greater_operator,
	&greater_equal_operator,
};
static CommandListOperatorFactoryBase *equality_operators[] = {
	&equality_operator,
	&inequality_operator,
	&identical_operator,
	&not_identical_operator,
};
static CommandListOperatorFactoryBase *and_operators[] = {
	&and_operator,
};
static CommandListOperatorFactoryBase *or_operators[] = {
	&or_operator,
};

static CommandListSyntaxTree::Tokens::iterator transform_operators_token(
		CommandListSyntaxTree *tree,
		CommandListSyntaxTree::Tokens::iterator i,
		CommandListOperatorFactoryBase *factories[], int num_factories,
		bool unary)
{
	std::shared_ptr<CommandListOperatorToken> token;
	std::shared_ptr<CommandListOperator> op;
	std::shared_ptr<CommandListOperandBase> lhs;
	std::shared_ptr<CommandListOperandBase> rhs;
	int f;

	token = dynamic_pointer_cast<CommandListOperatorToken>(*i);
	if (!token)
		return i;

	for (f = 0; f < num_factories; f++) {
		if (token->token.compare(factories[f]->pattern()))
			continue;

		lhs = nullptr;
		rhs = nullptr;
		if (i > tree->tokens.begin())
			lhs = dynamic_pointer_cast<CommandListOperandBase>(*(i-1));
		if (i < tree->tokens.end() - 1)
			rhs = dynamic_pointer_cast<CommandListOperandBase>(*(i+1));

		if (unary) {
			// It is particularly important that we check that the
			// LHS is *not* an operand so the unary +/- operators
			// don't trump the binary addition/subtraction operators:
			if (rhs && !lhs) {
				op = factories[f]->create(nullptr, *token, *(i+1));
				i = tree->tokens.erase(i, i+2);
				i = tree->tokens.insert(i, std::move(op));
				break;
			}
		} else {
			if (lhs && rhs) {
				op = factories[f]->create(*(i-1), *token, *(i+1));
				i = tree->tokens.erase(i-1, i+2);
				i = tree->tokens.insert(i, std::move(op));
				break;
			}
		}
	}

	return i;
}

// Transforms operator tokens in the syntax tree into actual operators
static void transform_operators_visit(CommandListSyntaxTree *tree,
		CommandListOperatorFactoryBase *factories[], int num_factories,
		bool right_associative, bool unary)
{
	CommandListSyntaxTree::Tokens::iterator i;
	CommandListSyntaxTree::Tokens::reverse_iterator rit;

	if (!tree)
		return;

	if (right_associative) {
		if (unary) {
			// Start at the second from the right
			for (rit = tree->tokens.rbegin() + 1; rit != tree->tokens.rend(); rit++) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				i = transform_operators_token(tree, rit.base() - 1, factories, num_factories, unary);
				rit = std::reverse_iterator<CommandListSyntaxTree::Tokens::iterator>(i + 1);
			}
		} else {
			for (rit = tree->tokens.rbegin() + 1; rit < tree->tokens.rend() - 1; rit++) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				i = transform_operators_token(tree, rit.base() - 1, factories, num_factories, unary);
				rit = std::reverse_iterator<CommandListSyntaxTree::Tokens::iterator>(i + 1);
			}
		}
	} else {
		if (unary) {
			throw CommandListSyntaxError(L"FIXME: Implement left-associative unary operators", 0);
		} else {
			// Since this is binary operators, skip the first and last
			// nodes as they must be operands, and this way I don't have to
			// worry about bounds checks.
			for (i = tree->tokens.begin() + 1; i < tree->tokens.end() - 1; i++)
				i = transform_operators_token(tree, i, factories, num_factories, unary);
		}
	}
}

static void transform_operators_recursive(CommandListWalkable *tree,
		CommandListOperatorFactoryBase *factories[], int num_factories,
		bool right_associative, bool unary)
{
	// Depth first to ensure that we have visited all sub-trees before
	// transforming operators in this level, since that may add new
	// sub-trees
	for (auto &inner: tree->walk()) {
		transform_operators_recursive(dynamic_cast<CommandListWalkable*>(inner.get()),
				factories, num_factories, right_associative, unary);
	}

	transform_operators_visit(dynamic_cast<CommandListSyntaxTree*>(tree),
			factories, num_factories, right_associative, unary);
}

// Using raw pointers here so that ::optimise() can call it with "this"
static void _log_syntax_tree(CommandListSyntaxTree *tree);
static void _log_token(CommandListToken *token)
{
	CommandListSyntaxTree *inner;
	CommandListOperator *op;
	CommandListOperatorToken *op_tok;
	CommandListOperand *operand;

	if (!token)
		return;

	// Can't use CommandListWalkable here, because it only walks over inner
	// syntax trees and this debug dumper needs to walk over everything

	inner = dynamic_cast<CommandListSyntaxTree*>(token);
	op = dynamic_cast<CommandListOperator*>(token);
	op_tok = dynamic_cast<CommandListOperatorToken*>(token);
	operand = dynamic_cast<CommandListOperand*>(token);
	if (inner) {
		_log_syntax_tree(inner);
	} else if (op) {
		LogInfoNoNL("Operator \"%S\"[ ", token->token.c_str());
		if (op->lhs_tree)
			_log_token(op->lhs_tree.get());
		else if (op->lhs)
			_log_token(dynamic_cast<CommandListToken*>(op->lhs.get()));
		if ((op->lhs_tree || op->lhs) && (op->rhs_tree || op->rhs))
			LogInfoNoNL(", ");
		if (op->rhs_tree)
			_log_token(op->rhs_tree.get());
		else if (op->rhs)
			_log_token(dynamic_cast<CommandListToken*>(op->rhs.get()));
		LogInfoNoNL(" ]");
	} else if (op_tok) {
		LogInfoNoNL("OperatorToken \"%S\"", token->token.c_str());
	} else if (operand) {
		LogInfoNoNL("Operand \"%S\"", token->token.c_str());
	} else {
		LogInfoNoNL("Token \"%S\"", token->token.c_str());
	}
}
static void _log_syntax_tree(CommandListSyntaxTree *tree)
{
	CommandListSyntaxTree::Tokens::iterator i;

	LogInfoNoNL("SyntaxTree[ ");
	for (i = tree->tokens.begin(); i != tree->tokens.end(); i++) {
		_log_token((*i).get());
		if (i != tree->tokens.end()-1)
			LogInfoNoNL(", ");
	}
	LogInfoNoNL(" ]");
}

static void log_syntax_tree(CommandListSyntaxTree *tree, const char *msg)
{
	if (!gLogDebug)
		return;

	LogInfo(msg);
	_log_syntax_tree(tree);
	LogInfo("\n");
}

template<class T>
static void log_syntax_tree(T token, const char *msg)
{
	if (!gLogDebug)
		return;

	LogInfo(msg);
	_log_token(dynamic_cast<CommandListToken*>(token.get()));
	LogInfo("\n");
}

bool CommandListExpression::parse(const wstring *expression, const wstring *ini_namespace, CommandListScope *scope)
{
	CommandListSyntaxTree tree(0);

	try {
		tokenise(expression, &tree, ini_namespace, scope);

		group_parenthesis(&tree);

		transform_operators_recursive(&tree, unary_operators, ARRAYSIZE(unary_operators), true, true);
		transform_operators_recursive(&tree, exponent_operators, ARRAYSIZE(exponent_operators), true, false);
		transform_operators_recursive(&tree, multi_division_operators, ARRAYSIZE(multi_division_operators), false, false);
		transform_operators_recursive(&tree, add_subtract_operators, ARRAYSIZE(add_subtract_operators), false, false);
		transform_operators_recursive(&tree, relational_operators, ARRAYSIZE(relational_operators), false, false);
		transform_operators_recursive(&tree, equality_operators, ARRAYSIZE(equality_operators), false, false);
		transform_operators_recursive(&tree, and_operators, ARRAYSIZE(and_operators), false, false);
		transform_operators_recursive(&tree, or_operators, ARRAYSIZE(or_operators), false, false);

		evaluatable = tree.finalise();
		log_syntax_tree(evaluatable, "Final syntax tree:\n");
		return true;
	} catch (const CommandListSyntaxError &e) {
		LogOverlay(LOG_WARNING_MONOSPACE,
				"Syntax Error: %S\n"
				"              %*s: %S\n",
				expression->c_str(), (int)e.pos+1, "^", e.msg.c_str());
		return false;
	}
}

float CommandListExpression::evaluate(CommandListState *state, HackerDevice *device)
{
	return evaluatable->evaluate(state, device);
}

bool CommandListExpression::static_evaluate(float *ret, HackerDevice *device)
{
	return evaluatable->static_evaluate(ret, device);
}

bool CommandListExpression::optimise(HackerDevice *device)
{
	std::shared_ptr<CommandListEvaluatable> replacement;
	bool ret;

	if (!evaluatable) {
		LogOverlay(LOG_DIRE, "BUG: Non-evaluatable expression, please report this and provide your d3dx.ini\n");
		evaluatable = std::make_shared<CommandListOperand>(0, L"<BUG>");
		return false;
	}

	ret = evaluatable->optimise(device, &replacement);

	if (replacement)
		evaluatable = replacement;

	return ret;
}

// Finalises the syntax trees in the operator into evaluatable operands,
// thereby making this operator also evaluatable.
std::shared_ptr<CommandListEvaluatable> CommandListOperator::finalise()
{
	auto lhs_finalisable = dynamic_pointer_cast<CommandListFinalisable>(lhs_tree);
	auto rhs_finalisable = dynamic_pointer_cast<CommandListFinalisable>(rhs_tree);
	auto lhs_evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(lhs_tree);
	auto rhs_evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(rhs_tree);

	if (lhs || rhs) {
		LogInfo("BUG: Attempted to finalise already final operator\n");
		throw CommandListSyntaxError(L"BUG", token_pos);
	}

	if (lhs_tree) { // Binary operators only
		if (!lhs && lhs_finalisable)
			lhs = lhs_finalisable->finalise();
		if (!lhs && lhs_evaluatable)
			lhs = lhs_evaluatable;
		if (!lhs)
			throw CommandListSyntaxError(L"BUG: LHS operand invalid", token_pos);
		lhs_tree = nullptr;
	}

	if (!rhs && rhs_finalisable)
		rhs = rhs_finalisable->finalise();
	if (!rhs && rhs_evaluatable)
		rhs = rhs_evaluatable;
	if (!rhs)
		throw CommandListSyntaxError(L"BUG: RHS operand invalid", token_pos);
	rhs_tree = nullptr;

	// Can't return "this", because that is an unmanaged version of the
	// pointer which is already managed elsewhere - if we were to create a
	// new managed pointer from that, we would have undefined behaviour.
	// Instead we just return nullptr to signify that this node does not
	// need to be replaced.
	return nullptr;
}

// Recursively finalises every node in the syntax tree. If the expression is
// valid the tree should be left with a single evaluatable node, which will be
// returned to the caller so that it can replace this tree with just the node.
// Throws a syntax error if the finalised nodes are not right.
std::shared_ptr<CommandListEvaluatable> CommandListSyntaxTree::finalise()
{
	std::shared_ptr<CommandListFinalisable> finalisable;
	std::shared_ptr<CommandListEvaluatable> evaluatable;
	std::shared_ptr<CommandListToken> token;
	Tokens::iterator i;

	for (i = tokens.begin(); i != tokens.end(); i++) {
		finalisable = dynamic_pointer_cast<CommandListFinalisable>(*i);
		if (finalisable) {
			evaluatable = finalisable->finalise();
			if (evaluatable) {
				// A recursive syntax tree has been finalised
				// and we replace it with its sole evaluatable
				// contents:
				token = dynamic_pointer_cast<CommandListToken>(evaluatable);
				if (!token) {
					LogInfo("BUG: finalised token did not cast back\n");
					throw CommandListSyntaxError(L"BUG", token_pos);
				}
				i = tokens.erase(i);
				i = tokens.insert(i, std::move(token));
			}
		}
	}

	// A finalised syntax tree should be reduced to a single evaluatable
	// operator/operand, which we pass back up the stack to replace this
	// tree
	if (tokens.empty())
		throw CommandListSyntaxError(L"Empty expression", 0);

	if (tokens.size() > 1)
		throw CommandListSyntaxError(L"Unexpected", tokens[1]->token_pos);

	evaluatable = dynamic_pointer_cast<CommandListEvaluatable>(tokens[0]);
	if (!evaluatable)
		throw CommandListSyntaxError(L"Non-evaluatable", tokens[0]->token_pos);

	return evaluatable;
}

CommandListSyntaxTree::Walk CommandListSyntaxTree::walk()
{
	Walk ret;
	std::shared_ptr<CommandListWalkable> inner;
	Tokens::iterator i;

	for (i = tokens.begin(); i != tokens.end(); i++) {
		inner = dynamic_pointer_cast<CommandListWalkable>(*i);
		if (inner)
			ret.push_back(std::move(inner));
	}

	return ret;
}

float CommandListOperator::evaluate(CommandListState *state, HackerDevice *device)
{
	if (lhs) // Binary operator
		return evaluate(lhs->evaluate(state, device), rhs->evaluate(state, device));
	return evaluate(std::numeric_limits<float>::quiet_NaN(), rhs->evaluate(state, device));
}

bool CommandListOperator::static_evaluate(float *ret, HackerDevice *device)
{
	float lhs_static = std::numeric_limits<float>::quiet_NaN(), rhs_static;
	bool is_static;

	is_static = rhs->static_evaluate(&rhs_static, device);
	if (lhs) // Binary operator
		is_static = lhs->static_evaluate(&lhs_static, device) && is_static;

	if (is_static) {
		if (ret)
			*ret = evaluate(lhs_static, rhs_static);
		return true;
	}

	return false;
}

bool CommandListOperator::optimise(HackerDevice *device, std::shared_ptr<CommandListEvaluatable> *replacement)
{
	std::shared_ptr<CommandListEvaluatable> lhs_replacement;
	std::shared_ptr<CommandListEvaluatable> rhs_replacement;
	shared_ptr<CommandListOperand> operand;
	bool making_progress = false;
	float static_val;
	wstring static_val_str;

	if (lhs)
		making_progress = lhs->optimise(device, &lhs_replacement) || making_progress;
	if (rhs)
		making_progress = rhs->optimise(device, &rhs_replacement) || making_progress;

	if (lhs_replacement)
		lhs = lhs_replacement;
	if (rhs_replacement)
		rhs = rhs_replacement;

	if (!static_evaluate(&static_val, device))
		return making_progress;

	// FIXME: Pretty print rather than dumping syntax tree
	LogInfoNoNL("Statically evaluated \"");
	_log_token(dynamic_cast<CommandListToken*>(this));
	LogInfo("\" as %f\n", static_val);
	static_val_str = std::to_wstring(static_val);

	operand = make_shared<CommandListOperand>(token_pos, static_val_str.c_str());
	operand->type = ParamOverrideType::VALUE;
	operand->val = static_val;
	*replacement = dynamic_pointer_cast<CommandListEvaluatable>(operand);
	return true;
}

CommandListSyntaxTree::Walk CommandListOperator::walk()
{
	Walk ret;
	std::shared_ptr<CommandListWalkable> lhs;
	std::shared_ptr<CommandListWalkable> rhs;

	lhs = dynamic_pointer_cast<CommandListWalkable>(lhs_tree);
	rhs = dynamic_pointer_cast<CommandListWalkable>(rhs_tree);

	if (lhs)
		ret.push_back(std::move(lhs));
	if (rhs)
		ret.push_back(std::move(rhs));

	return ret;
}

void ParamOverride::run(CommandListState *state)
{
	float *dest = &(G->iniParams[param_idx].*param_component);
	float orig = *dest;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	*dest = expression.evaluate(state);

	COMMAND_LIST_LOG(state, "  ini param override = %f\n", *dest);

	state->update_params |= (*dest != orig);
}

void VariableAssignment::run(CommandListState *state)
{
	float orig = var->fval;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	var->fval = expression.evaluate(state);

	COMMAND_LIST_LOG(state, "  = %f\n", var->fval);

	if (var->flags & VariableFlags::PERSIST)
		G->user_config_dirty |= (var->fval != orig);
}

bool AssignmentCommand::optimise(HackerDevice *device)
{
	return expression.optimise(device);
}

static bool operand_allowed_in_context(ParamOverrideType type, CommandListScope *scope)
{
	if (scope)
		return true;

	// List of operand types allowed outside of a command list, e.g. in a
	// [Key] / [Preset] section
	switch (type) {
		case ParamOverrideType::VALUE:
		case ParamOverrideType::INI_PARAM:
		case ParamOverrideType::VARIABLE:
		case ParamOverrideType::RES_WIDTH:
		case ParamOverrideType::RES_HEIGHT:
		case ParamOverrideType::TIME:
		case ParamOverrideType::RAW_SEPARATION:
		case ParamOverrideType::CONVERGENCE:
		case ParamOverrideType::EYE_SEPARATION:
		case ParamOverrideType::STEREO_ACTIVE:
		case ParamOverrideType::STEREO_AVAILABLE:
		case ParamOverrideType::SLI:
		case ParamOverrideType::HUNTING:
		case ParamOverrideType::EFFECTIVE_DPI:
			return true;
	}
	return false;
}

bool valid_variable_name(const wstring &name)
{
	if (name.length() < 2)
		return false;

	// Variable names begin with a $
	if (name[0] != L'$')
		return false;

	// First character must be a letter or underscore ($1, $2, etc reserved for future arguments):
	if ((name[1] < L'a' || name[1] > L'z') && name[1] != L'_')
		return false;

	// Subsequent characters must be in this list:
	return (name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz_0123456789", 2) == wstring::npos);
}

bool parse_command_list_var_name(const wstring &name, const wstring *ini_namespace, CommandListVariable **target)
{
	CommandListVariables::iterator var = command_list_globals.end();

	if (name.length() < 2 || name[0] != L'$')
		return false;

	// We need value in lower case so our keys will be consistent in the
	// unordered_map. ParseCommandList will have already done this, but the
	// Key/Preset parsing code will not have, and rather than require it to
	// we do it here:
	wstring low_name(name);
	std::transform(low_name.begin(), low_name.end(), low_name.begin(), ::towlower);

	var = command_list_globals.end();
	if (!ini_namespace->empty())
		var = command_list_globals.find(get_namespaced_var_name_lower(low_name, ini_namespace));
	if (var == command_list_globals.end())
		var = command_list_globals.find(low_name);
	if (var == command_list_globals.end())
		return false;

	*target = &var->second;
	return true;
}

int find_local_variable(const wstring &name, CommandListScope *scope, CommandListVariable **var)
{
	CommandListScope::iterator it;

	if (!scope)
		return false;

	if (name.length() < 2 || name[0] != L'$')
		return false;

	for (it = scope->begin(); it != scope->end(); it++) {
		auto match = it->find(name);
		if (match != it->end()) {
			*var = match->second;
			return true;
		}
	}

	return false;
}

bool declare_local_variable(const wchar_t *section, wstring &name,
		CommandList *pre_command_list, const wstring *ini_namespace)
{
	CommandListVariable *var = NULL;

	if (!valid_variable_name(name)) {
		LogOverlay(LOG_WARNING, "WARNING: Illegal local variable name: [%S] \"%S\"\n", section, name.c_str());
		return false;
	}

	if (find_local_variable(name, pre_command_list->scope, &var)) {
		// Could allow this at different scope levels, but... no.
		// You can declare local variables of the same name in
		// independent scopes (if {local $tmp} else {local $tmp}), but
		// we won't allow masking a local variable from a parent scope,
		// because that's usually a bug. Choose a different name son.
		LogOverlay(LOG_WARNING, "WARNING: Illegal redeclaration of local variable [%S] %S\n", section, name.c_str());
		return false;
	}

	if (parse_command_list_var_name(name, ini_namespace, &var)) {
		// Not making this fatal since this could clash between say a
		// global in the d3dx.ini and a local variable in another ini.
		// Just issue a notice in hunting mode and carry on.
		LogOverlay(LOG_NOTICE, "WARNING: [%S] local %S masks a global variable with the same name\n", section, name.c_str());
	}

	pre_command_list->static_vars.emplace_front(name, 0.0f, VariableFlags::NONE);
	pre_command_list->scope->front()[name] = &pre_command_list->static_vars.front();

	return true;
}

bool CommandListOperand::parse(const wstring *operand, const wstring *ini_namespace, CommandListScope *scope)
{
	CommandListVariable *var = NULL;
	int ret, len1;

	// Try parsing value as a float
	ret = swscanf_s(operand->c_str(), L"%f%n", &val, &len1);
	if (ret != 0 && ret != EOF && len1 == operand->length()) {
		type = ParamOverrideType::VALUE;
		return operand_allowed_in_context(type, scope);
	}

	// Try parsing operand as an ini param:
	if (ParseIniParamName(operand->c_str(), &param_idx, &param_component)) {
		type = ParamOverrideType::INI_PARAM;
		// Reserve space in IniParams for this variable:
		G->iniParamsReserved = max(G->iniParamsReserved, param_idx + 1);
		return operand_allowed_in_context(type, scope);
	}

	// Try parsing operand as a variable:
	if (find_local_variable(*operand, scope, &var) ||
	    parse_command_list_var_name(*operand, ini_namespace, &var)) {
		type = ParamOverrideType::VARIABLE;
		var_ftarget = &var->fval;
		return operand_allowed_in_context(type, scope);
	}

	// Try parsing value as a resource target for texture filtering
	ret = texture_filter_target.ParseTarget(operand->c_str(), true, ini_namespace);
	if (ret) {
		type = ParamOverrideType::TEXTURE;
		return operand_allowed_in_context(type, scope);
	}

	// Try parsing value as a shader target for partner filtering
	// WARNING: This test is especially susceptible to an uninitialised
	//          %n fooling it into thinking it has parsed the entire string
	//          if the stack garbage happens to contain operand->length().
	//          This is because the %n does not immediately follow another
	//          conversion specification and does not alter the return
	//          value, so the return value will not distinguish between
	//          early termination and completion, and since %lc will match
	//          any character this can trigger easily. Seems to only occur
	//          on vs2013, though I'm not positive if vs2017 zeroes out
	//          len1 or dumb luck gave different values in the stack.
	len1 = 0;
	ret = swscanf_s(operand->c_str(), L"%lcs%n", &shader_filter_target, 1, &len1);
	if (ret == 1 && len1 == operand->length()) {
		switch(shader_filter_target) {
		case L'v': case L'h': case L'd': case L'g': case L'p': case L'c':
			type = ParamOverrideType::SHADER;
			return operand_allowed_in_context(type, scope);
		}
	}

	// Try parsing value as a scissor rectangle. scissor_<side> also
	// appears in the keywords list for uses of the default rectangle 0.
	len1 = 0;
	ret = swscanf_s(operand->c_str(), L"scissor%u_%n", &scissor, &len1);
	if (ret == 1 && scissor < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
		if (!wcscmp(operand->c_str() + len1, L"left"))
			type = ParamOverrideType::SCISSOR_LEFT;
		else if (!wcscmp(operand->c_str() + len1, L"top"))
			type = ParamOverrideType::SCISSOR_TOP;
		else if (!wcscmp(operand->c_str() + len1, L"right"))
			type = ParamOverrideType::SCISSOR_RIGHT;
		else if (!wcscmp(operand->c_str() + len1, L"bottom"))
			type = ParamOverrideType::SCISSOR_BOTTOM;
		else
			return false;
		return operand_allowed_in_context(type, scope);
	}

	// Check special keywords
	type = lookup_enum_val<const wchar_t *, ParamOverrideType>
		(ParamOverrideTypeNames, operand->c_str(), ParamOverrideType::INVALID);
	if (type != ParamOverrideType::INVALID)
		return operand_allowed_in_context(type, scope);

	return false;
}

// Parse IniParams overrides, in forms such as
// x = 0.3 (set parameter to specific value, e.g. for shader partner filtering)
// y2 = ps-t0 (use parameter for texture filtering based on texture slot of shader type)
// z3 = rt_width / rt_height (set parameter to render target width/height)
// w4 = res_width / res_height (set parameter to resolution width/height)
bool ParseCommandListIniParamOverride(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace)
{
	ParamOverride *param = new ParamOverride();

	if (!ParseIniParamName(key, &param->param_idx, &param->param_component))
		goto bail;

	if (!param->expression.parse(val, ini_namespace, command_list->scope))
		goto bail;

	// Reserve space in IniParams for this variable:
	G->iniParamsReserved = max(G->iniParamsReserved, param->param_idx + 1);

	param->ini_line = L"[" + wstring(section) + L"] " + wstring(key) + L" = " + *val;
	command_list->commands.push_back(std::shared_ptr<CommandListCommand>(param));
	return true;
bail:
	delete param;
	return false;
}

bool ParseCommandListVariableAssignment(const wchar_t *section,
		const wchar_t *key, wstring *val, const wstring *raw_line,
		CommandList *command_list, CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	VariableAssignment *command = NULL;
	CommandListVariable *var = NULL;
	wstring name = key;

	// Declaration without assignment?
	if (name.empty() && raw_line)
		name = *raw_line;

	if (!name.compare(0, 6, L"local ")) {
		name = name.substr(name.find_first_not_of(L" \t", 6));
		// Local variables are shared between pre and post command lists.
		if (!declare_local_variable(section, name, pre_command_list, ini_namespace))
			return false;

		// Declaration without assignment?
		if (val->empty())
			return true;
	}

	if (!find_local_variable(name, pre_command_list->scope, &var) &&
	    !parse_command_list_var_name(name, ini_namespace, &var))
		return false;

	command = new VariableAssignment();
	command->var = var;

	if (!command->expression.parse(val, ini_namespace, command_list->scope))
		goto bail;

	command->ini_line = L"[" + wstring(section) + L"] " + wstring(key) + L" = " + *val;
	command_list->commands.push_back(std::shared_ptr<CommandListCommand>(command));
	return true;
bail:
	delete command;
	return false;
}

ResourcePool::~ResourcePool()
{
	ResourcePoolCache::iterator i;

	for (i = cache.begin(); i != cache.end(); i++) {
		if (i->second.first)
			i->second.first->Release();
	}
	cache.clear();
}

void ResourcePool::emplace(uint32_t hash, ID3D11Resource *resource, ID3D11Device *device)
{
	if (resource)
		resource->AddRef();
	cache.emplace(hash, pair<ID3D11Resource*, ID3D11Device*>{resource, device});
}

template <typename ResourceType,
	 typename DescType,
	HRESULT (__stdcall ID3D11Device::*CreateResource)(THIS_
	      const DescType *pDesc,
	      const D3D11_SUBRESOURCE_DATA *pInitialData,
	      ResourceType **ppTexture)
	>
static ResourceType* GetResourceFromPool(
		wstring *ini_line,
		ResourceType *src_resource,
		ResourceType *dst_resource,
		ResourcePool *resource_pool,
		CommandListState *state,
		DescType *desc)
{
	ResourceType *resource = NULL;
	DescType old_desc;
	uint32_t hash;
	size_t size;
	HRESULT hr;
	ResourcePoolCache::iterator pool_i;
	ID3D11Device *old_device = NULL;

	// We don't want to use the CalTexture2D/3DDescHash functions because
	// the resolution override could produce the same hash for distinct
	// texture descriptions. This hash isn't exposed to the user, so
	// doesn't matter what we use - just has to be fast.
	hash = crc32c_hw(0, desc, sizeof(DescType));

	pool_i = Profiling::lookup_map(resource_pool->cache, hash, &Profiling::resource_pool_lookup_overhead);
	if (pool_i != resource_pool->cache.end()) {
		resource = (ResourceType*)pool_i->second.first;
		old_device = pool_i->second.second;
		if (!resource)
			return NULL;

		if (old_device == state->mOrigDevice1) {
			if (resource == dst_resource)
				return NULL;

			LogDebug("Switching cached resource %S\n", ini_line->c_str());
			Profiling::resource_pool_swaps++;
			resource->AddRef();
			return resource;
		}

		LogInfo("Device mismatch, discarding %S from resource pool\n", ini_line->c_str());
		resource_pool->cache.erase(pool_i);
		resource->Release();
	}

	LogInfo("Creating cached resource %S\n", ini_line->c_str());
	Profiling::resources_created++;

	hr = (state->mOrigDevice1->*CreateResource)(desc, NULL, &resource);
	if (FAILED(hr)) {
		LogInfo("Resource copy failed %S: 0x%x\n", ini_line->c_str(), hr);
		LogResourceDesc(desc);
		src_resource->GetDesc(&old_desc);
		LogInfo("Original resource was:\n");
		LogResourceDesc(&old_desc);

		// Prevent further attempts:
		resource_pool->emplace(hash, NULL, NULL);

		return NULL;
	}
	resource_pool->emplace(hash, resource, state->mOrigDevice1);
	size = resource_pool->cache.size();
	if (size > 1)
		LogInfo("  NOTICE: cache now contains %Ii resources\n", size);

	LogDebugResourceDesc(desc);
	return resource;
}

CustomResource::CustomResource() :
	resource(NULL),
	device(NULL),
	view(NULL),
	is_null(true),
	substantiated(false),
	bind_flags((D3D11_BIND_FLAG)0),
	misc_flags((D3D11_RESOURCE_MISC_FLAG)0),
	stride(0),
	offset(0),
	buf_size(0),
	format(DXGI_FORMAT_UNKNOWN),
	max_copies_per_frame(0),
	frame_no(0),
	copies_this_frame(0),
	override_type(CustomResourceType::INVALID),
	override_mode(CustomResourceMode::DEFAULT),
	override_bind_flags(CustomResourceBindFlags::INVALID),
	override_misc_flags(ResourceMiscFlags::INVALID),
	override_format((DXGI_FORMAT)-1),
	override_width(-1),
	override_height(-1),
	override_depth(-1),
	override_mips(-1),
	override_array(-1),
	override_msaa(-1),
	override_msaa_quality(-1),
	override_byte_width(-1),
	override_stride(-1),
	width_multiply(1.0f),
	height_multiply(1.0f),
	initial_data(NULL),
	initial_data_size(0)
{}

CustomResource::~CustomResource()
{
	if (resource)
		resource->Release();
	if (view)
		view->Release();
	free(initial_data);
}

bool CustomResource::OverrideSurfaceCreationMode(StereoHandle mStereoHandle, NVAPI_STEREO_SURFACECREATEMODE *orig_mode)
{

	if (override_mode == CustomResourceMode::DEFAULT)
		return false;

	Profiling::NvAPI_Stereo_GetSurfaceCreationMode(mStereoHandle, orig_mode);

	switch (override_mode) {
		case CustomResourceMode::STEREO:
			Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);
			return true;
		case CustomResourceMode::MONO:
			Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCEMONO);
			return true;
		case CustomResourceMode::AUTO:
			Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_AUTO);
			return true;
	}

	return false;
}

void CustomResource::Substantiate(ID3D11Device *mOrigDevice1, StereoHandle mStereoHandle,
		D3D11_BIND_FLAG bind_flags, D3D11_RESOURCE_MISC_FLAG misc_flags)
{
	NVAPI_STEREO_SURFACECREATEMODE orig_mode = NVAPI_STEREO_SURFACECREATEMODE_AUTO;
	bool restore_create_mode = false;

	// We only allow a custom resource to be substantiated once. Otherwise
	// we could end up reloading it again if it is later set to null. Also
	// prevents us from endlessly retrying to load a custom resource from a
	// file that doesn't exist:
	if (substantiated)
		return;
	substantiated = true;

	// If this custom resource has already been set through other means we
	// won't overwrite it:
	if (resource || view)
		return;

	Profiling::resources_created++;

	// Add any extra bind flags necessary for the current assignment. This
	// covers many (but not all) of the cases 3DMigoto cannot deduce
	// earlier during parsing - it will cover custom resources copied by
	// reference to other custom resources when the "this" resource target
	// is assigned. There are some complicated cases that could still need
	// bind_flags to be manually specified - where multiple bind flags are
	// required and cannot be deduced at parse time.
	this->bind_flags = (D3D11_BIND_FLAG)(this->bind_flags | bind_flags);
	this->misc_flags = (D3D11_RESOURCE_MISC_FLAG)(this->misc_flags | misc_flags);

	// If the resource section has enough information to create a resource
	// we do so the first time it is loaded from. The reason we do it this
	// late is to make sure we know which device is actually being used to
	// render the game - FC4 creates about a dozen devices with different
	// parameters while probing the hardware before it settles on the one
	// it will actually use.

	LockResourceCreationMode();

	restore_create_mode = OverrideSurfaceCreationMode(mStereoHandle, &orig_mode);

	if (!filename.empty()) {
		LoadFromFile(mOrigDevice1);
	} else {
		switch (override_type) {
			case CustomResourceType::BUFFER:
			case CustomResourceType::STRUCTURED_BUFFER:
			case CustomResourceType::RAW_BUFFER:
				SubstantiateBuffer(mOrigDevice1, NULL, 0);
				break;
			case CustomResourceType::TEXTURE1D:
				SubstantiateTexture1D(mOrigDevice1);
				break;
			case CustomResourceType::TEXTURE2D:
			case CustomResourceType::CUBE:
				SubstantiateTexture2D(mOrigDevice1);
				break;
			case CustomResourceType::TEXTURE3D:
				SubstantiateTexture3D(mOrigDevice1);
				break;
		}
	}

	if (restore_create_mode)
		Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, orig_mode);

	UnlockResourceCreationMode();
}

void CustomResource::LoadBufferFromFile(ID3D11Device *mOrigDevice1)
{
	DWORD size, read_size;
	void *buf = NULL;
	HANDLE f;

	f = CreateFile(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		LogOverlay(LOG_WARNING, "Failed to load custom buffer resource %S: %d\n", filename.c_str(), GetLastError());
		return;
	}

	size = GetFileSize(f, 0);
	buf = malloc(size); // malloc to allow realloc to resize it if the user overrode the size
	if (!buf) {
		LogOverlay(LOG_DIRE, "Out of memory loading %S\n", filename.c_str());
		goto out_close;
	}

	if (!ReadFile(f, buf, size, &read_size, 0) || size != read_size) {
		LogOverlay(LOG_WARNING, "Error reading custom buffer from file %S\n", filename.c_str());
		goto out_delete;
	}

	SubstantiateBuffer(mOrigDevice1, &buf, size);

out_delete:
	free(buf);
out_close:
	CloseHandle(f);
}

void CustomResource::LoadFromFile(ID3D11Device *mOrigDevice1)
{
	wstring ext;
	HRESULT hr;

	switch (override_type) {
		case CustomResourceType::BUFFER:
		case CustomResourceType::STRUCTURED_BUFFER:
		case CustomResourceType::RAW_BUFFER:
			return LoadBufferFromFile(mOrigDevice1);
	}

	// This code path doesn't get a chance to override the resource
	// description, since DirectXTK takes care of that, but we can pass in
	// bind flags at least, which is sometimes necessary in complex
	// situations where 3DMigoto cannot automatically determine these or
	// when manipulating driver heuristics:
	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		bind_flags = (D3D11_BIND_FLAG)override_bind_flags;
	if (override_misc_flags != ResourceMiscFlags::INVALID)
		misc_flags = (D3D11_RESOURCE_MISC_FLAG)override_misc_flags;

	// XXX: We are not creating a view with DirecXTK because
	// 1) it assumes we want a shader resource view, which is an
	//    assumption that doesn't fit with the goal of this code to
	//    allow for arbitrary resource copying, and
	// 2) we currently won't use the view in a source custom
	//    resource, even if we are referencing it into a compatible
	//    slot. We might improve this, and if we do, I don't want
	//    any surprises caused by a view of the wrong type we
	//    happen to have created here and forgotten about.
	// If we do start using the source custom resource's view, we
	// could do something smart here, like only using it if the
	// bind_flags indicate it will be used as a shader resource.

	ext = filename.substr(filename.rfind(L"."));
	if (!_wcsicmp(ext.c_str(), L".dds")) {
		LogInfoW(L"Loading custom resource %s as DDS, bind_flags=0x%03x\n", filename.c_str(), bind_flags);
		hr = DirectX::CreateDDSTextureFromFileEx(mOrigDevice1,
				filename.c_str(), 0,
				D3D11_USAGE_DEFAULT, bind_flags, 0, misc_flags,
				DirectX::DX11::DDS_LOADER_DEFAULT, &resource, NULL, NULL);
	} else {
		LogInfoW(L"Loading custom resource %s as WIC, bind_flags=0x%03x\n", filename.c_str(), bind_flags);
		hr = DirectX::CreateWICTextureFromFileEx(mOrigDevice1,
				filename.c_str(), 0,
				D3D11_USAGE_DEFAULT, bind_flags, 0, misc_flags,
				DirectX::DX11::WIC_LOADER_DEFAULT, &resource, NULL);
	}
	if (SUCCEEDED(hr)) {
		device = mOrigDevice1;
		is_null = false;
		// TODO:
		// format = ...
	} else
		LogOverlay(LOG_WARNING, "Failed to load custom texture resource %S: 0x%x\n", filename.c_str(), hr);
}

void CustomResource::SubstantiateBuffer(ID3D11Device *mOrigDevice1, void **buf, DWORD size)
{
	D3D11_SUBRESOURCE_DATA data = {0}, *pInitialData = NULL;
	ID3D11Buffer *buffer;
	D3D11_BUFFER_DESC desc;
	HRESULT hr;

	if (!buf) {
		// If no file is passed in, we use the optional initial data to
		// initialise the buffer. We do this even if no initial data
		// has been specified, so that the buffer will be initialised
		// with zeroes for safety.
		buf = &initial_data;
		size = (DWORD)initial_data_size;
	}

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.MiscFlags = misc_flags;

	// Allow the buffer size to be set from the file / initial data size,
	// but it can be overridden if specified explicitly. If it's a
	// structured buffer, we assume just a single structure by default, but
	// again this can be overridden. The reason for doing this here, and
	// not in OverrideBufferDesc, is that this only applies if we are
	// substantiating the resource from scratch, not when copying a resource.
	if (size) {
		desc.ByteWidth = size;
		if (override_type == CustomResourceType::STRUCTURED_BUFFER)
			desc.StructureByteStride = size;
	}

	OverrideBufferDesc(&desc);

	if (desc.ByteWidth > 0) {
		// Fill in size from the file/initial data, allowing for an
		// override to make it larger or smaller, which may involve
		// reallocating the buffer from the caller.
		if (desc.ByteWidth > size) {
			void *new_buf = realloc(*buf, desc.ByteWidth);
			if (!new_buf) {
				LogInfo("Out of memory enlarging buffer: [%S]\n", name.c_str());
				return;
			}
			memset((char*)new_buf + size, 0, desc.ByteWidth - size);
			*buf = new_buf;
		}

		data.pSysMem = *buf;
		pInitialData = &data;
	}

	hr = mOrigDevice1->CreateBuffer(&desc, pInitialData, &buffer);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S], bind_flags=0x%03x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), desc.BindFlags);
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)buffer;
		device = mOrigDevice1;
		is_null = false;
		OverrideOutOfBandInfo(&format, &stride);
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture1D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture1D *tex1d;
	D3D11_TEXTURE1D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.MiscFlags = misc_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture1D(&desc, NULL, &tex1d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S], bind_flags=0x%03x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), desc.BindFlags);
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex1d;
		device = mOrigDevice1;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture2D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture2D *tex2d;
	D3D11_TEXTURE2D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.MiscFlags = misc_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture2D(&desc, NULL, &tex2d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S], bind_flags=0x%03x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), desc.BindFlags);
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex2d;
		device = mOrigDevice1;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}
void CustomResource::SubstantiateTexture3D(ID3D11Device *mOrigDevice1)
{
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE3D_DESC desc;
	HRESULT hr;

	memset(&desc, 0, sizeof(desc));
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.MiscFlags = misc_flags;
	OverrideTexDesc(&desc);

	hr = mOrigDevice1->CreateTexture3D(&desc, NULL, &tex3d);
	if (SUCCEEDED(hr)) {
		LogInfo("Substantiated custom %S [%S], bind_flags=0x%03x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), desc.BindFlags);
		LogDebugResourceDesc(&desc);
		resource = (ID3D11Resource*)tex3d;
		device = mOrigDevice1;
		is_null = false;
	} else {
		LogOverlay(LOG_NOTICE, "Failed to substantiate custom %S [%S]: 0x%x\n",
				lookup_enum_name(CustomResourceTypeNames, override_type), name.c_str(), hr);
		LogResourceDesc(&desc);
	}
}

void CustomResource::OverrideBufferDesc(D3D11_BUFFER_DESC *desc)
{
	switch (override_type) {
		case CustomResourceType::STRUCTURED_BUFFER:
			desc->MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			break;
		case CustomResourceType::RAW_BUFFER:
			desc->MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			break;
	}

	if (override_stride != -1)
		desc->StructureByteStride = override_stride;
	else if (override_format != (DXGI_FORMAT)-1 && override_format != DXGI_FORMAT_UNKNOWN)
		desc->StructureByteStride = dxgi_format_size(override_format);

	if (override_byte_width != -1)
		desc->ByteWidth = override_byte_width;
	else if (override_array != -1)
		desc->ByteWidth = desc->StructureByteStride * override_array;

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;
	if (override_misc_flags != ResourceMiscFlags::INVALID)
		desc->MiscFlags = (D3D11_RESOURCE_MISC_FLAG)override_misc_flags;
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE1D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_array != -1)
		desc->ArraySize = override_array;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;

	desc->Width = (UINT)(desc->Width * width_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;
	if (override_misc_flags != ResourceMiscFlags::INVALID)
		desc->MiscFlags = (D3D11_RESOURCE_MISC_FLAG)override_misc_flags;
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE2D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_height != -1)
		desc->Height = override_height;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;
	if (override_array != -1)
		desc->ArraySize = override_array;
	if (override_msaa != -1)
		desc->SampleDesc.Count = override_msaa;
	if (override_msaa_quality != -1)
		desc->SampleDesc.Quality = override_msaa_quality;

	if (override_type == CustomResourceType::CUBE) {
		desc->MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
		if (override_array != -1)
			desc->ArraySize = override_array * 6;
	}

	desc->Width = (UINT)(desc->Width * width_multiply);
	desc->Height = (UINT)(desc->Height * height_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;
	if (override_misc_flags != ResourceMiscFlags::INVALID)
		desc->MiscFlags = (D3D11_RESOURCE_MISC_FLAG)override_misc_flags;
}

void CustomResource::OverrideTexDesc(D3D11_TEXTURE3D_DESC *desc)
{
	if (override_width != -1)
		desc->Width = override_width;
	if (override_height != -1)
		desc->Height = override_height;
	if (override_depth != -1)
		desc->Height = override_depth;
	if (override_mips != -1)
		desc->MipLevels = override_mips;
	if (override_format != (DXGI_FORMAT)-1)
		desc->Format = override_format;

	desc->Width = (UINT)(desc->Width * width_multiply);
	desc->Height = (UINT)(desc->Height * height_multiply);

	if (override_bind_flags != CustomResourceBindFlags::INVALID)
		desc->BindFlags = (D3D11_BIND_FLAG)override_bind_flags;
	if (override_misc_flags != ResourceMiscFlags::INVALID)
		desc->MiscFlags = (D3D11_RESOURCE_MISC_FLAG)override_misc_flags;
}

void CustomResource::OverrideOutOfBandInfo(DXGI_FORMAT *format, UINT *stride)
{
	if (override_format != (DXGI_FORMAT)-1)
		*format = override_format;
	if (override_stride != -1)
		*stride = override_stride;
}

// Returns 1 for definite dsv formats
// Returns 2 for typeless variants of dsv formats (have a stencil buffer)
// Returns -1 for possible typecast depth formats, but with no stencil buffer they may not be
static int is_dsv_format(DXGI_FORMAT fmt)
{
	switch(fmt)
	{
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_D16_UNORM:
			return 1;
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return 2;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_UNORM:
			return -1;
		default:
			return 0;
	}
}


// Transfer a resource from one device to another. First came across this
// possibility in DOAXVV where showing the news creates a new temporary device
// & context, and due to another bug [Constants] would be re-run, which allowed
// certain mods to copy one resource to another crashing the game. For that
// game we want to disallow re-running [Constants], but the possibility of
// running into more of these issues remains, particularly given that many
// games tend to create & destroy devices on startup where we would actually
// prefer the final device to own resources (in contrast to DOAXVV where we
// want the first device to remain the owner). This attempts to transfer
// resources between devices on demand so that whichever device is currently in
// use will own the resources, however this is very slow and if it ever crops
// up in a fast path we'd need to look for ways to avoid this.
//
// This does not handle all edge cases (depth buffers, dynamic destinations and
// MSAA are not supported by UpdateSubresource), and knowing DirectX there will
// probably be more undocumented surprise combinations that don't work either.
//
// TODO: Refactor this - the four cases are very similar with some differences
//
static ID3D11Resource * inter_device_resource_transfer(ID3D11Device *dst_dev, ID3D11DeviceContext *dst_ctx, ID3D11Resource *src_res, wstring *name)
{
	ID3D11Device *src_dev = NULL;
	ID3D11DeviceContext *src_ctx = NULL;
	ID3D11Resource *stg_res = NULL;
	ID3D11Resource *dtg_res = NULL;
	ID3D11Resource *dst_res = NULL;
	D3D11_RESOURCE_DIMENSION dimension;
	D3D11_MAPPED_SUBRESOURCE src_map;
	UINT item, level, index;
	const char *reason = "";

	Profiling::inter_device_copies++;

	// Not really anything sensible we can do with the resource creation
	// mode in the general case here as yet - if we copy via the CPU we
	// will lose the 2nd perspective, but we have no way to even know if
	// one even exists. There are some limited situations we could copy a
	// stereo texture between devices (reverse stereo blit -> stereo
	// header), but let's wait until we have a proven need before we try
	// anything heroic that probably won't work anyway. Tbh we might be
	// better off just trying to avoid getting down this code path - maybe
	// just discarding resources for re-substantiation and re-running
	// Contants will be sufficient in most cases
	LockResourceCreationMode();

	src_res->GetDevice(&src_dev);
	reason = "Source device unavailable\n";
	if (!src_dev)
		goto err;

	src_dev->GetImmediateContext(&src_ctx);
	reason = "Source context unavailable\n";
	if (!src_ctx)
		goto err;

	src_res->GetType(&dimension);
	switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
		{
			ID3D11Buffer *buf = (ID3D11Buffer*)src_res;
			D3D11_BUFFER_DESC buf_desc;
			buf->GetDesc(&buf_desc);

			reason = "Dynamic Buffers unsupported\n";
			if (buf_desc.Usage == D3D11_USAGE_DYNAMIC)
				goto err;
			if (buf_desc.Usage == D3D11_USAGE_IMMUTABLE)
				buf_desc.Usage = D3D11_USAGE_DEFAULT;

			dst_dev->CreateBuffer(&buf_desc, NULL, (ID3D11Buffer**)&dst_res);
			if (!dst_res) {
				reason = "Error creating final destination Buffer\n";
				LogResourceDesc(&buf_desc);
				goto err;
			}

			buf_desc.Usage = D3D11_USAGE_STAGING;
			buf_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			buf_desc.BindFlags = 0;
			src_dev->CreateBuffer(&buf_desc, NULL, (ID3D11Buffer**)&stg_res);
			if (!stg_res) {
				reason = "Error creating source staging Buffer\n";
				LogResourceDesc(&buf_desc);
				goto err;
			}
			src_ctx->CopyResource(stg_res, src_res);

			// Buffers only have a single subresource:
			index = 0;
			reason = "Error mapping source staging Buffer\n";
			if (FAILED(src_ctx->Map(stg_res, index, D3D11_MAP_READ, 0, &src_map)))
				goto err;
			dst_ctx->UpdateSubresource(dst_res, index, NULL, src_map.pData, src_map.RowPitch, src_map.DepthPitch);
			src_ctx->Unmap(stg_res, index);
			break;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
		{
			ID3D11Texture1D *tex1d = (ID3D11Texture1D*)src_res;
			D3D11_TEXTURE1D_DESC tex1d_desc;
			tex1d->GetDesc(&tex1d_desc);

			reason = "Dynamic Texture1Ds unsupported\n";
			if (tex1d_desc.Usage == D3D11_USAGE_DYNAMIC)
				goto err;
			if (tex1d_desc.Usage == D3D11_USAGE_IMMUTABLE)
				tex1d_desc.Usage = D3D11_USAGE_DEFAULT;

			// It's not clear to me from the UpdateSubresource documentation if the
			// reason depth/stencil buffers aren't supported is down to the bind flags,
			// or the format... For now, erring on the side of throwing an error either
			// way, but trying to continue if it is just the format
			if ((tex1d_desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
				reason = "depth/stencil buffers unsupported";
				goto err;
			}
			if (is_dsv_format(tex1d_desc.Format) > 0) {
				LogOverlay(LOG_NOTICE, "Inter-device transfer of [%S] with depth/stencil format %s may or may not work. Please report success/failure.\n",
						name->c_str(), TexFormatStr(tex1d_desc.Format));
			}

			dst_dev->CreateTexture1D(&tex1d_desc, NULL, (ID3D11Texture1D**)&dst_res);
			if (!dst_res) {
				reason = "Error creating final destination Texture1D\n";
				LogResourceDesc(&tex1d_desc);
				goto err;
			}

			tex1d_desc.Usage = D3D11_USAGE_STAGING;
			tex1d_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			tex1d_desc.BindFlags = 0;
			tex1d_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
			src_dev->CreateTexture1D(&tex1d_desc, NULL, (ID3D11Texture1D**)&stg_res);
			if (!stg_res) {
				reason = "Error creating staging Texture1D\n";
				LogResourceDesc(&tex1d_desc);
				goto err;
			}
			src_ctx->CopyResource(stg_res, src_res);

			for (item = 0; item < tex1d_desc.ArraySize; item++) {
				for (level = 0; level < tex1d_desc.MipLevels; level++) {
					index = D3D11CalcSubresource(level, item, max(tex1d_desc.MipLevels, 1));
					reason = "Error mapping source staging Texture1D\n";
					if (FAILED(src_ctx->Map(stg_res, index, D3D11_MAP_READ, 0, &src_map)))
						goto err;
					dst_ctx->UpdateSubresource(dst_res, index, NULL, src_map.pData, src_map.RowPitch, src_map.DepthPitch);
					src_ctx->Unmap(stg_res, index);
				}
			}
			break;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
		{
			ID3D11Texture2D *tex2d = (ID3D11Texture2D*)src_res;
			D3D11_TEXTURE2D_DESC tex2d_desc;
			tex2d->GetDesc(&tex2d_desc);

			reason = "Dynamic Texture2Ds unsupported\n";
			if (tex2d_desc.Usage == D3D11_USAGE_DYNAMIC)
				goto err;
			if (tex2d_desc.Usage == D3D11_USAGE_IMMUTABLE)
				tex2d_desc.Usage = D3D11_USAGE_DEFAULT;

			if (tex2d_desc.SampleDesc.Count > 1) {
				// Not sure how to handle this one - I'd be surprised if it works for map/unmap
				// and while we can resolve MSAA (for supported formats) on the source device,
				// what do we do to restore the lost samples on the destination?
				reason = "MSAA resources unsupported";
				goto err;
			}

			// It's not clear to me from the UpdateSubresource documentation if the
			// reason depth/stencil buffers aren't supported is down to the bind flags,
			// or the format... For now, erring on the side of throwing an error either
			// way, but trying to continue if it is just the format
			if ((tex2d_desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
				reason = "depth/stencil buffers unsupported";
				goto err;
			}
			if (is_dsv_format(tex2d_desc.Format) > 0) {
				LogOverlay(LOG_NOTICE, "Inter-device transfer of [%S] with depth/stencil format %s may or may not work. Please report success/failure.\n",
						name->c_str(), TexFormatStr(tex2d_desc.Format));
			}

			dst_dev->CreateTexture2D(&tex2d_desc, NULL, (ID3D11Texture2D**)&dst_res);
			if (!dst_res) {
				reason = "Error creating final destination Texture2D\n";
				LogResourceDesc(&tex2d_desc);
				goto err;
			}

			tex2d_desc.Usage = D3D11_USAGE_STAGING;
			tex2d_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			tex2d_desc.BindFlags = 0;
			tex2d_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
			src_dev->CreateTexture2D(&tex2d_desc, NULL, (ID3D11Texture2D**)&stg_res);
			if (!stg_res) {
				reason = "Error creating staging Texture2D\n";
				LogResourceDesc(&tex2d_desc);
				goto err;
			}
			src_ctx->CopyResource(stg_res, src_res);

			for (item = 0; item < tex2d_desc.ArraySize; item++) {
				for (level = 0; level < tex2d_desc.MipLevels; level++) {
					index = D3D11CalcSubresource(level, item, max(tex2d_desc.MipLevels, 1));
					reason = "Error mapping source staging Texture2D\n";
					if (FAILED(src_ctx->Map(stg_res, index, D3D11_MAP_READ, 0, &src_map)))
						goto err;
					dst_ctx->UpdateSubresource(dst_res, index, NULL, src_map.pData, src_map.RowPitch, src_map.DepthPitch);
					src_ctx->Unmap(stg_res, index);
				}
			}
			break;
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
		{
			ID3D11Texture3D *tex3d = (ID3D11Texture3D*)src_res;
			D3D11_TEXTURE3D_DESC tex3d_desc;
			tex3d->GetDesc(&tex3d_desc);

			reason = "Dynamic Texture3Ds unsupported\n";
			if (tex3d_desc.Usage == D3D11_USAGE_DYNAMIC)
				goto err;
			if (tex3d_desc.Usage == D3D11_USAGE_IMMUTABLE)
				tex3d_desc.Usage = D3D11_USAGE_DEFAULT;

			dst_dev->CreateTexture3D(&tex3d_desc, NULL, (ID3D11Texture3D**)&dst_res);
			if (!dst_res) {
				reason = "Error creating final destination Texture3D\n";
				LogResourceDesc(&tex3d_desc);
				goto err;
			}

			tex3d_desc.Usage = D3D11_USAGE_STAGING;
			tex3d_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			tex3d_desc.BindFlags = 0;
			tex3d_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
			src_dev->CreateTexture3D(&tex3d_desc, NULL, (ID3D11Texture3D**)&stg_res);
			if (!stg_res) {
				reason = "Error creating staging Texture3D\n";
				LogResourceDesc(&tex3d_desc);
				goto err;
			}
			src_ctx->CopyResource(stg_res, src_res);

			for (level = 0; level < tex3d_desc.MipLevels; level++) {
				// 3D Textures cannot be arrays, so only mip-map level counts for subresource index:
				index = level;
				reason = "Error mapping source staging Texture3D\n";
				if (FAILED(src_ctx->Map(stg_res, index, D3D11_MAP_READ, 0, &src_map)))
					goto err;
				dst_ctx->UpdateSubresource(dst_res, index, NULL, src_map.pData, src_map.RowPitch, src_map.DepthPitch);
				src_ctx->Unmap(stg_res, index);
			}
			break;
		}
		default:
			reason = "Unknown dimension\n";
			goto err;
	}

out:
	if (stg_res)
		stg_res->Release();
	if (src_ctx)
		src_ctx->Release();
	if (src_dev)
		src_dev->Release();
	UnlockResourceCreationMode();
	return dst_res;

err:
	LogOverlay(LOG_DIRE, "Inter-device transfer of [%S] failed: %s\n", name->c_str(), reason);
	if (dst_res)
		dst_res->Release();
	dst_res = NULL;
	goto out;
}

void CustomResource::expire(ID3D11Device *mOrigDevice1, ID3D11DeviceContext *mOrigContext1)
{
	ID3D11Resource *new_resource = NULL;

	if (!resource || is_null)
		return;

	// Check for device mismatches and handle via expiring cached
	// resources, flagging for re-substantiation and/or performing
	// inter-device transfers if necessary. We cache the device rather than
	// querying it from the resource via GetDevice(), because if ReShade is
	// in use GetDevice() will return the real device, but we will compare
	// it with the ReShade device and think there has been a device swap
	// when there has not been.
	if (device == mOrigDevice1)
		return;

	// Attempt to transfer resource to new device by staging to the CPU and
	// back. Rather slow, but ensures the contents are up to date.
	// TODO: Search other custom resources for references to this resource
	//       and update those. Not urgent as yet, but will be important if
	//       we allow the === and !== operators to check if one custom
	//       resource matches another.
	// TODO: Track if resource is dirty (used as render/depth target/UAV,
	//       ever assigned or copied to) and re-substantiate if clean.
	// TODO: Use sharable resources to skip copy back to CPU (limited to 2D
	//       non-mip-mapped resources)
	// TODO: Option to skip inter-device copies instead of transfer
	// TODO: Option to discard individual resources instead of transfer
	// TODO: Option to discard all custom resources and re-run [Constants]
	//       on new device (for convoluted startup sequences)
	LogInfo("Device mismatch, transferring [%S] to new device\n", name.c_str());
	new_resource = inter_device_resource_transfer(
			mOrigDevice1, mOrigContext1, resource, &name);

	// Expire cache:
	resource->Release();
	if (view)
		view->Release();
	view = NULL;

	if (new_resource) {
		// Inter-device copy succeeded, switch to the new resource:
		resource = new_resource;
		device = mOrigDevice1;
	} else {
		// Inter-device copy failed / skipped. Flag resource for
		// re-substantiation (if possible for this resource):
		substantiated = false;
		resource = NULL;
		device = NULL;
		is_null = true;
	}
}

bool ResourceCopyTarget::ParseTarget(const wchar_t *target,
		bool is_source, const wstring *ini_namespace)
{
	int ret, len;
	size_t length = wcslen(target);
	CustomResources::iterator res;

	ret = swscanf_s(target, L"%lcs-cb%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
		type = ResourceCopyTargetType::CONSTANT_BUFFER;
		goto check_shader_type;
	}

	ret = swscanf_s(target, L"%lcs-t%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
		type = ResourceCopyTargetType::SHADER_RESOURCE;
	       goto check_shader_type;
	}

	// TODO: ret = swscanf_s(target, L"%lcs-s%u%n", &shader_type, 1, &slot, &len);
	// TODO: if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::SAMPLER;
	// TODO:	goto check_shader_type;
	// TODO: }

	ret = swscanf_s(target, L"o%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
		type = ResourceCopyTargetType::RENDER_TARGET;
		return true;
	}

	if (!wcscmp(target, L"od")) {
		type = ResourceCopyTargetType::DEPTH_STENCIL_TARGET;
		return true;
	}

	ret = swscanf_s(target, L"%lcs-u%u%n", &shader_type, 1, &slot, &len);
	// XXX: On Win8 D3D11_1_UAV_SLOT_COUNT (64) is the limit instead. Use
	// the lower amount for now to enforce compatibility.
	if (ret == 2 && len == length && slot < D3D11_PS_CS_UAV_REGISTER_COUNT) {
		// These views are only valid for pixel and compute shaders:
		if (shader_type == L'p' || shader_type == L'c') {
			type = ResourceCopyTargetType::UNORDERED_ACCESS_VIEW;
			return true;
		}
		return false;
	}

	ret = swscanf_s(target, L"vb%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
		type = ResourceCopyTargetType::VERTEX_BUFFER;
		return true;
	}

	if (!wcscmp(target, L"ib")) {
		type = ResourceCopyTargetType::INDEX_BUFFER;
		return true;
	}

	ret = swscanf_s(target, L"so%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_SO_STREAM_COUNT) {
		type = ResourceCopyTargetType::STREAM_OUTPUT;
		return true;
	}

	if (is_source && !wcscmp(target, L"null")) {
		type = ResourceCopyTargetType::EMPTY;
		return true;
	}

	if (length >= 9 && !wcsncmp(target, L"resource", 8)) {
		// section name should already have been transformed to lower
		// case from ParseCommandList, so our keys will be consistent
		// in the unordered_map:
		wstring resource_id(target);
		wstring namespaced_section;

		res = customResources.end();
		if (get_namespaced_section_name_lower(&resource_id, ini_namespace, &namespaced_section))
			res = customResources.find(namespaced_section);
		if (res == customResources.end())
			res = customResources.find(resource_id);
		if (res == customResources.end())
			return false;

		custom_resource = &res->second;
		type = ResourceCopyTargetType::CUSTOM_RESOURCE;
		return true;
	}

	// Alternate means to assign StereoParams and IniParams
	if (is_source && !wcscmp(target, L"stereoparams")) {
		type = ResourceCopyTargetType::STEREO_PARAMS;
		return true;
	}

	if (is_source && !wcscmp(target, L"iniparams")) {
		type = ResourceCopyTargetType::INI_PARAMS;
		return true;
	}

	if (is_source && !wcscmp(target, L"cursor_mask")) {
		type = ResourceCopyTargetType::CURSOR_MASK;
		return true;
	}

	if (is_source && !wcscmp(target, L"cursor_color")) {
		type = ResourceCopyTargetType::CURSOR_COLOR;
		return true;
	}

	if (!wcscmp(target, L"this")) {
		type = ResourceCopyTargetType::THIS_RESOURCE;
		return true;
	}

	// XXX: Any reason to allow access to sequential swap chains? Given
	// they either won't exist or are read only I can't think of one.
	if (is_source && !wcscmp(target, L"bb")) { // Back Buffer
		type = ResourceCopyTargetType::SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	if (is_source && !wcscmp(target, L"r_bb")) {
		type = ResourceCopyTargetType::REAL_SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	if (is_source && !wcscmp(target, L"f_bb")) {
		type = ResourceCopyTargetType::FAKE_SWAP_CHAIN;
		// Holding a reference on the back buffer will prevent
		// ResizeBuffers() from working, so forbid caching any views of
		// the back buffer. Leaving it bound could also be a problem,
		// but since this is usually only used from custom shader
		// sections they will take care of unbinding it automatically:
		forbid_view_cache = true;
		return true;
	}

	return false;

check_shader_type:
	switch(shader_type) {
		case L'v': case L'h': case L'd': case L'g': case L'p': case L'c':
			return true;
	}
	return false;
}


bool ParseCommandListResourceCopyDirective(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace)
{
	ResourceCopyOperation *operation = new ResourceCopyOperation();
	wchar_t buf[MAX_PATH];
	wchar_t *src_ptr = NULL;
	D3D11_RESOURCE_MISC_FLAG misc_flags = (D3D11_RESOURCE_MISC_FLAG)0;

	if (!operation->dst.ParseTarget(key, false, ini_namespace))
		goto bail;

	// parse_enum_option_string replaces spaces with NULLs, so it can't
	// operate on the buffer in the wstring directly. I could potentially
	// change it to work without modifying the string, but for now it's
	// easier to just make a copy of the string:
	if (val->length() >= MAX_PATH)
		goto bail;
	wcsncpy_s(buf, val->c_str(), MAX_PATH);

	operation->options = parse_enum_option_string<wchar_t *, ResourceCopyOptions>
		(ResourceCopyOptionNames, buf, &src_ptr);

	if (!src_ptr)
		goto bail;

	if (!operation->src.ParseTarget(src_ptr, true, ini_namespace))
		goto bail;

	if (!(operation->options & ResourceCopyOptions::COPY_TYPE_MASK)) {
		// If the copy method was not speficied make a guess.
		// References aren't always safe (e.g. a resource can't be both
		// an input and an output), and a resource may not have been
		// created with the right usage flags, so we'll err on the side
		// of doing a full copy if we aren't fairly sure.
		//
		// If we're merely copying a resource from one shader to
		// another without changnig the usage (e.g. giving the vertex
		// shader access to a constant buffer or texture from the pixel
		// shader) a reference is probably safe (unless the game
		// reassigns it to a different usage later and doesn't know
		// that our reference is still bound somewhere), but it would
		// not be safe to give a vertex shader access to the depth
		// buffer of the output merger stage, for example.
		//
		// If we are copying a resource into a custom resource (e.g.
		// for use from another draw call), do a full copy by default
		// in case the game alters the original.
		//
		// If we are assigning a render target, do so by reference
		// since we probably want the result reflected in the resource
		// we assigned to it. Mostly this would already work due to the
		// custom resource rules, but adding this rule should make
		// assigning the back buffer to a render target work.
		if (operation->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
			operation->options |= ResourceCopyOptions::COPY;
		else if (operation->dst.type == ResourceCopyTargetType::RENDER_TARGET)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->src.type == operation->dst.type)
			operation->options |= ResourceCopyOptions::REFERENCE;
		else if (operation->dst.type == ResourceCopyTargetType::SHADER_RESOURCE
				&& (operation->src.type == ResourceCopyTargetType::STEREO_PARAMS
				|| operation->src.type == ResourceCopyTargetType::INI_PARAMS
				|| operation->src.type == ResourceCopyTargetType::CURSOR_MASK
				|| operation->src.type == ResourceCopyTargetType::CURSOR_COLOR))
			operation->options |= ResourceCopyOptions::REFERENCE;
		else
			operation->options |= ResourceCopyOptions::COPY;
	}

	// FIXME: If custom resources are copied to other custom resources by
	// reference that are in turn bound to the pipeline we may not
	// propagate all the bind flags correctly depending on the order
	// everything is parsed. We'd need to construct a dependency graph
	// to fix this, but it's not clear that this combination would really
	// be used in practice, so for now this will do. Update: We now
	// or in the bind flags in use when the custom resource is first
	// Substantiate()ed, which will cover most of these missing cases - the
	// remaining exceptions would be where differing bind flags are used at
	// different times and we still can't deduce it here
	// FIXME: The constant buffer bind flag can't be combined with others
	if (operation->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE &&
			(operation->options & ResourceCopyOptions::REFERENCE)) {
		// Fucking C++ making this line 3x longer than it should be:
		operation->src.custom_resource->bind_flags = (D3D11_BIND_FLAG)
			(operation->src.custom_resource->bind_flags | operation->dst.BindFlags(NULL, &misc_flags));
		operation->src.custom_resource->misc_flags = (D3D11_RESOURCE_MISC_FLAG)
			(operation->src.custom_resource->misc_flags | misc_flags);
	}

	operation->ini_line = L"[" + wstring(section) + L"] " + wstring(key) + L" = " + *val;
	command_list->commands.push_back(std::shared_ptr<CommandListCommand>(operation));
	return true;
bail:
	delete operation;
	return false;
}

static bool ParseIfCommand(const wchar_t *section, const wstring *line,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	IfCommand *operation = new IfCommand(section);
	wstring expression = line->substr(line->find_first_not_of(L" \t", 3));

	if (!operation->expression.parse(&expression, ini_namespace, pre_command_list->scope))
		goto bail;

	// New scope level to isolate local variables:
	pre_command_list->scope->emplace_front();

	return AddCommandToList(operation, NULL, NULL, pre_command_list, post_command_list, section, line->c_str(), NULL);
bail:
	delete operation;
	return false;
}

static bool ParseElseIfCommand(const wchar_t *section, const wstring *line, int prefix,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	ElseIfCommand *operation = new ElseIfCommand(section);
	wstring expression = line->substr(line->find_first_not_of(L" \t", prefix));

	if (!operation->expression.parse(&expression, ini_namespace, pre_command_list->scope))
		goto bail;

	// Clear deepest scope level to isolate local variables:
	pre_command_list->scope->front().clear();

	// "else if" is implemented by nesting another if/endif inside the
	// parent if command's else clause. We add both an ElsePlaceholder and
	// an ElseIfCommand here, and will fix up the "endif" balance later.
	AddCommandToList(new ElsePlaceholder(), NULL, NULL, pre_command_list, post_command_list, section, line->c_str(), NULL);
	return AddCommandToList(operation, NULL, NULL, pre_command_list, post_command_list, section, line->c_str(), NULL);
bail:
	delete operation;
	return false;
}

static bool ParseElseCommand(const wchar_t *section,
		CommandList *pre_command_list, CommandList *post_command_list)
{
	// Clear deepest scope level to isolate local variables:
	pre_command_list->scope->front().clear();

	return AddCommandToList(new ElsePlaceholder(), NULL, NULL, pre_command_list, post_command_list, section, L"else", NULL);
}

static bool _ParseEndIfCommand(const wchar_t *section,
		CommandList *command_list, bool post, bool has_nested_else_if = false)
{
	CommandList::Commands::reverse_iterator rit;
	IfCommand *if_command;
	ElseIfCommand *else_if_command;
	ElsePlaceholder *else_command = NULL;
	CommandList::Commands::iterator else_pos = command_list->commands.end();

	for (rit = command_list->commands.rbegin(); rit != command_list->commands.rend(); rit++) {
		else_command = dynamic_cast<ElsePlaceholder*>(rit->get());
		if (else_command) {
			// C++ gotcha: reverse_iterator::base() points to the *next* element
			else_pos = rit.base() - 1;
		}

		if_command = dynamic_cast<IfCommand*>(rit->get());
		if (if_command) {
			// "else if" is treated as embedding another "if" block
			// in the else clause of the first "if" command. The
			// ElsePlaceholder was already inserted when the
			// ElseIfCommand was parsed, and this ElseIfCommand acts
			// as the nested IfCommand so the below code works as
			// is, but to balance the endifs we will need to repeat
			// this function until we find the original IfCommand:
			else_if_command = dynamic_cast<ElseIfCommand*>(rit->get());

			// Transfer the commands since the if command until the
			// endif into the if command's true/false lists
			if (post && !if_command->post_finalised) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				if_command->true_commands_post->commands.assign(rit.base(), else_pos);
				if_command->true_commands_post->ini_section = if_command->ini_line;
				if (else_pos != command_list->commands.end()) {
					// Discard the else placeholder command:
					if_command->false_commands_post->commands.assign(else_pos + 1, command_list->commands.end());
					if_command->false_commands_post->ini_section = if_command->ini_line + L" <else>";
				}
				command_list->commands.erase(rit.base(), command_list->commands.end());
				if_command->post_finalised = true;
				if_command->has_nested_else_if = has_nested_else_if;
				if (else_if_command)
					return _ParseEndIfCommand(section, command_list, post, true);
				return true;
			} else if (!post && !if_command->pre_finalised) {
				// C++ gotcha: reverse_iterator::base() points to the *next* element
				if_command->true_commands_pre->commands.assign(rit.base(), else_pos);
				if_command->true_commands_pre->ini_section = if_command->ini_line;
				if (else_pos != command_list->commands.end()) {
					// Discard the else placeholder command:
					if_command->false_commands_pre->commands.assign(else_pos + 1, command_list->commands.end());
					if_command->false_commands_pre->ini_section = if_command->ini_line + L" <else>";
				}
				command_list->commands.erase(rit.base(), command_list->commands.end());
				if_command->pre_finalised = true;
				if_command->has_nested_else_if = has_nested_else_if;
				if (else_if_command)
					return _ParseEndIfCommand(section, command_list, post, true);
				return true;
			}
		}
	}

	LogOverlay(LOG_WARNING, "WARNING: [%S] endif missing if\n", section);
	return false;
}

static bool ParseEndIfCommand(const wchar_t *section,
		CommandList *pre_command_list, CommandList *post_command_list)
{
	bool ret;

	ret = _ParseEndIfCommand(section, pre_command_list, false);
	if (post_command_list)
	    ret = ret && _ParseEndIfCommand(section, post_command_list, true);

	if (ret)
		pre_command_list->scope->pop_front();

	return ret;
}

bool ParseCommandListFlowControl(const wchar_t *section, const wstring *line,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace)
{
	if (!wcsncmp(line->c_str(), L"if ", 3))
		return ParseIfCommand(section, line, pre_command_list, post_command_list, ini_namespace);
	if (!wcsncmp(line->c_str(), L"elif ", 5))
		return ParseElseIfCommand(section, line, 5, pre_command_list, post_command_list, ini_namespace);
	if (!wcsncmp(line->c_str(), L"else if ", 8))
		return ParseElseIfCommand(section, line, 8, pre_command_list, post_command_list, ini_namespace);
	if (!wcscmp(line->c_str(), L"else"))
		return ParseElseCommand(section, pre_command_list, post_command_list);
	if (!wcscmp(line->c_str(), L"endif"))
		return ParseEndIfCommand(section, pre_command_list, post_command_list);

	return false;
}

IfCommand::IfCommand(const wchar_t *section) :
	pre_finalised(false),
	post_finalised(false),
	has_nested_else_if(false),
	section(section)
{
	true_commands_pre = std::make_shared<CommandList>();
	true_commands_post = std::make_shared<CommandList>();
	false_commands_pre = std::make_shared<CommandList>();
	false_commands_post = std::make_shared<CommandList>();
	true_commands_post->post = true;
	false_commands_post->post = true;

	// Placeholder names to be replaced by endif processing - we should
	// never see these, but in case they do show up somewhere these will
	// provide a clue as to what they are:
	true_commands_pre->ini_section = L"if placeholder";
	true_commands_post->ini_section = L"if placeholder";
	false_commands_pre->ini_section = L"else placeholder";
	false_commands_post->ini_section = L"else placeholder";

	// Place the dynamically allocated command lists in this data structure
	// to ensure they stay alive until after the optimisation stage, even
	// if the IfCommand is freed, e.g. by being optimised out:
	dynamically_allocated_command_lists.push_back(true_commands_pre);
	dynamically_allocated_command_lists.push_back(true_commands_post);
	dynamically_allocated_command_lists.push_back(false_commands_pre);
	dynamically_allocated_command_lists.push_back(false_commands_post);

	// And register these command lists for later optimisation:
	registered_command_lists.push_back(true_commands_pre.get());
	registered_command_lists.push_back(true_commands_post.get());
	registered_command_lists.push_back(false_commands_pre.get());
	registered_command_lists.push_back(false_commands_post.get());
}

void IfCommand::run(CommandListState *state)
{
	if (expression.evaluate(state)) {
		COMMAND_LIST_LOG(state, "%S: true {\n", ini_line.c_str());
		state->extra_indent++;
		if (state->post)
			_RunCommandList(true_commands_post.get(), state, false);
		else
			_RunCommandList(true_commands_pre.get(), state, false);
		state->extra_indent--;
		COMMAND_LIST_LOG(state, "} endif\n");
	} else {
		COMMAND_LIST_LOG(state, "%S: false\n", ini_line.c_str());
		if (!has_nested_else_if) {
			COMMAND_LIST_LOG(state, "[%S] else {\n", section.c_str());
			state->extra_indent++;
		}
		if (state->post)
			_RunCommandList(false_commands_post.get(), state, false);
		else
			_RunCommandList(false_commands_pre.get(), state, false);
		if (!has_nested_else_if) {
			state->extra_indent--;
			COMMAND_LIST_LOG(state, "} endif\n");
		}
	}
}

bool IfCommand::optimise(HackerDevice *device)
{
	return expression.optimise(device);
}

bool IfCommand::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	float static_val;
	bool is_static;

	if ((post && !post_finalised) || (!post && !pre_finalised)) {
		LogOverlay(LOG_WARNING, "WARNING: If missing endif: %S\n", ini_line.c_str());
		return true;
	}

	is_static = expression.static_evaluate(&static_val);
	if (is_static) {
		if (static_val) {
			false_commands_pre->clear();
			false_commands_post->clear();
		} else {
			true_commands_pre->clear();
			true_commands_post->clear();
		}
	}

	if (post)
		return true_commands_post->commands.empty() && false_commands_post->commands.empty();
	return true_commands_pre->commands.empty() && false_commands_pre->commands.empty();
}

void CommandPlaceholder::run(CommandListState*)
{
	LogOverlay(LOG_DIRE, "BUG: Placeholder command executed: %S\n", ini_line.c_str());
}

bool CommandPlaceholder::noop(bool post, bool ignore_cto_pre, bool ignore_cto_post)
{
	LogOverlay(LOG_WARNING, "WARNING: Command not terminated: %S\n", ini_line.c_str());
	return true;
}

ID3D11Resource *ResourceCopyTarget::GetResource(
		CommandListState *state,
		ID3D11View **view,   // Used by textures, render targets, depth/stencil buffers & UAVs
		UINT *stride,        // Used by vertex buffers
		UINT *offset,        // Used by vertex & index buffers
		DXGI_FORMAT *format, // Used by index buffers
		UINT *buf_size,      // Used when creating a view of the buffer
		ResourceCopyTarget *dst) // Used to get bind flags when substantiating a custom resource
{
	HackerDevice *mHackerDevice = state->mHackerDevice;
	ID3D11Device *mOrigDevice1 = state->mOrigDevice1;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Resource *res = NULL;
	ID3D11Buffer *buf = NULL;
	ID3D11Buffer *so_bufs[D3D11_SO_STREAM_COUNT];
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;
	D3D11_BIND_FLAG bind_flags = (D3D11_BIND_FLAG)0;
	D3D11_RESOURCE_MISC_FLAG misc_flags = (D3D11_RESOURCE_MISC_FLAG)0;
	unsigned i;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'h':
			mOrigContext1->HSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'd':
			mOrigContext1->DSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'g':
			mOrigContext1->GSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'p':
			mOrigContext1->PSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'c':
			mOrigContext1->CSGetConstantBuffers(slot, 1, &buf);
			return buf;
		default:
			// Should not happen
			return NULL;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext1->HSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext1->DSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext1->GSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext1->PSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext1->CSGetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return NULL;
		}

		if (!resource_view)
			return NULL;

		resource_view->GetResource(&res);
		if (!res) {
			resource_view->Release();
			return NULL;
		}

		*view = resource_view;
		return res;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;

	case ResourceCopyTargetType::VERTEX_BUFFER:
		// TODO: If copying this to a constant buffer, provide some
		// means to get the strides + offsets from within the shader.
		// Perhaps as an IniParam, or in another constant buffer?
		mOrigContext1->IAGetVertexBuffers(slot, 1, &buf, stride, offset);
		return buf;

	case ResourceCopyTargetType::INDEX_BUFFER:
		// TODO: Similar comment as vertex buffers above, provide a
		// means for a shader to get format + offset.
		mOrigContext1->IAGetIndexBuffer(&buf, format, offset);
		if (stride && format)
			*stride = dxgi_format_size(*format);
		return buf;

	case ResourceCopyTargetType::STREAM_OUTPUT:
		// XXX: Does not give us the offset
		mOrigContext1->SOGetTargets(slot + 1, so_bufs);

		// Release any buffers we aren't after:
		for (i = 0; i < slot; i++) {
			if (so_bufs[i]) {
				so_bufs[i]->Release();
				so_bufs[i] = NULL;
			}
		}

		return so_bufs[slot];

	case ResourceCopyTargetType::RENDER_TARGET:
		mOrigContext1->OMGetRenderTargets(slot + 1, render_view, NULL);

		// Release any views we aren't after:
		for (i = 0; i < slot; i++) {
			if (render_view[i]) {
				render_view[i]->Release();
				render_view[i] = NULL;
			}
		}

		if (!render_view[slot])
			return NULL;

		render_view[slot]->GetResource(&res);
		if (!res) {
			render_view[slot]->Release();
			return NULL;
		}

		*view = render_view[slot];
		return res;

	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		mOrigContext1->OMGetRenderTargets(0, NULL, &depth_view);
		if (!depth_view)
			return NULL;

		depth_view->GetResource(&res);
		if (!res) {
			depth_view->Release();
			return NULL;
		}

		// Depth buffers can't be buffers

		*view = depth_view;
		return res;

	case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
		switch(shader_type) {
		case L'p':
			// XXX: Not clear if the start slot is ok like this from the docs?
			// Particularly, what happens if we retrieve a subsequent UAV?
			mOrigContext1->OMGetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, slot, 1, &unordered_view);
			break;
		case L'c':
			mOrigContext1->CSGetUnorderedAccessViews(slot, 1, &unordered_view);
			break;
		default:
			// Should not happen
			return NULL;
		}

		if (!unordered_view)
			return NULL;

		unordered_view->GetResource(&res);
		if (!res) {
			unordered_view->Release();
			return NULL;
		}

		*view = unordered_view;
		return res;

	case ResourceCopyTargetType::CUSTOM_RESOURCE:
		custom_resource->expire(mOrigDevice1, mOrigContext1);

		if (dst)
			bind_flags = dst->BindFlags(state, &misc_flags);
		custom_resource->Substantiate(mOrigDevice1, mHackerDevice->mStereoHandle, bind_flags, misc_flags);

		if (stride)
			*stride = custom_resource->stride;
		if (offset)
			*offset = custom_resource->offset;
		if (format)
			*format = custom_resource->format;
		if (buf_size)
			*buf_size = custom_resource->buf_size;

		if (custom_resource->is_null) {
			// Optimisation to allow the resource to be set to null
			// without throwing away the cache so we don't
			// endlessly create & destroy temporary resources.
			*view = NULL;
			return NULL;
		}

		if (custom_resource->view)
			custom_resource->view->AddRef();
		*view = custom_resource->view;
		if (custom_resource->resource)
			custom_resource->resource->AddRef();
		return custom_resource->resource;

	case ResourceCopyTargetType::STEREO_PARAMS:
		if (mHackerDevice->mStereoResourceView)
			mHackerDevice->mStereoResourceView->AddRef();
		*view = mHackerDevice->mStereoResourceView;
		if (mHackerDevice->mStereoTexture)
			mHackerDevice->mStereoTexture->AddRef();
		return mHackerDevice->mStereoTexture;

	case ResourceCopyTargetType::INI_PARAMS:
		if (mHackerDevice->mIniResourceView)
			mHackerDevice->mIniResourceView->AddRef();
		*view = mHackerDevice->mIniResourceView;
		if (mHackerDevice->mIniTexture)
			mHackerDevice->mIniTexture->AddRef();
		return mHackerDevice->mIniTexture;

	case ResourceCopyTargetType::CURSOR_MASK:
		UpdateCursorResources(state);
		if (state->cursor_mask_view)
			state->cursor_mask_view->AddRef();
		*view = state->cursor_mask_view;
		if (state->cursor_mask_tex)
			state->cursor_mask_tex->AddRef();
		return state->cursor_mask_tex;

	case ResourceCopyTargetType::CURSOR_COLOR:
		UpdateCursorResources(state);
		if (state->cursor_color_view)
			state->cursor_color_view->AddRef();
		*view = state->cursor_color_view;
		if (state->cursor_color_tex)
			state->cursor_color_tex->AddRef();
		return state->cursor_color_tex;

	case ResourceCopyTargetType::THIS_RESOURCE:
		if (state->this_target)
			return state->this_target->GetResource(state, view, stride, offset, format, buf_size);

		if (state->resource) {
			if (state->view)
				state->view->AddRef();
			*view = state->view;
			if (*state->resource)
				(*state->resource)->AddRef();
			return (*state->resource);
		}

		COMMAND_LIST_LOG(state, "  \"this\"  is not valid in this context\n");
		return NULL;

	case ResourceCopyTargetType::SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain) {
				if (G->bb_is_upscaling_bb)
					mHackerSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
				else
					mHackerSwapChain->GetOrigSwapChain1()->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			} else
				COMMAND_LIST_LOG(state, "  Unable to get access to swap chain\n");
		}
		return res;

	case ResourceCopyTargetType::REAL_SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain)
				mHackerSwapChain->GetOrigSwapChain1()->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			else
				COMMAND_LIST_LOG(state, "  Unable to get access to real swap chain\n");
		}
		return res;

	case ResourceCopyTargetType::FAKE_SWAP_CHAIN:
		{
			HackerSwapChain *mHackerSwapChain = mHackerDevice->GetHackerSwapChain();
			if (mHackerSwapChain)
				mHackerSwapChain->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&res);
			else
				COMMAND_LIST_LOG(state, "  Unable to get access to fake swap chain\n");
		}
		return res;
	}

	return NULL;
}

void ResourceCopyTarget::SetResource(
		CommandListState *state,
		ID3D11Resource *res,
		ID3D11View *view,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_size)
{
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Buffer *buf = NULL;
	ID3D11Buffer *so_bufs[D3D11_SO_STREAM_COUNT];
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	ID3D11UnorderedAccessView *unordered_view = NULL;
	UINT uav_counter = -1; // TODO: Allow this to be set
	int i;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		buf = (ID3D11Buffer*)res;
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'h':
			mOrigContext1->HSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'd':
			mOrigContext1->DSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'g':
			mOrigContext1->GSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'p':
			mOrigContext1->PSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'c':
			mOrigContext1->CSSetConstantBuffers(slot, 1, &buf);
			return;
		default:
			// Should not happen
			return;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		resource_view = (ID3D11ShaderResourceView*)view;
		switch(shader_type) {
		case L'v':
			mOrigContext1->VSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext1->HSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext1->DSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext1->GSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext1->PSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext1->CSSetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return;
		}
		break;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;

	case ResourceCopyTargetType::VERTEX_BUFFER:
		buf = (ID3D11Buffer*)res;
		mOrigContext1->IASetVertexBuffers(slot, 1, &buf, &stride, &offset);
		return;

	case ResourceCopyTargetType::INDEX_BUFFER:
		buf = (ID3D11Buffer*)res;
		mOrigContext1->IASetIndexBuffer(buf, format, offset);
		break;

	case ResourceCopyTargetType::STREAM_OUTPUT:
		// XXX: HERE BE UNTESTED CODE PATHS!
		buf = (ID3D11Buffer*)res;
		mOrigContext1->SOGetTargets(D3D11_SO_STREAM_COUNT, so_bufs);
		if (so_bufs[slot])
			so_bufs[slot]->Release();
		so_bufs[slot] = buf;
		// XXX: We set offsets to NULL here. We should really preserve
		// them, but I'm not sure how to get their original values,
		// so... too bad. Probably will never even use this anyway.
		mOrigContext1->SOSetTargets(D3D11_SO_STREAM_COUNT, so_bufs, NULL);

		for (i = 0; i < D3D11_SO_STREAM_COUNT; i++) {
			if (so_bufs[i])
				so_bufs[i]->Release();
		}

		break;

	case ResourceCopyTargetType::RENDER_TARGET:
		mOrigContext1->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, &depth_view);

		if (render_view[slot])
			render_view[slot]->Release();
		render_view[slot] = (ID3D11RenderTargetView*)view;

		mOrigContext1->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, depth_view);

		for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			if (i != slot && render_view[i])
				render_view[i]->Release();
		}
		if (depth_view)
			depth_view->Release();

		break;

	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		mOrigContext1->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, &depth_view);

		if (depth_view)
			depth_view->Release();
		depth_view = (ID3D11DepthStencilView*)view;

		mOrigContext1->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, depth_view);

		for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			if (render_view[i])
				render_view[i]->Release();
		}
		break;

	case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
		// XXX: HERE BE UNTESTED CODE PATHS!
		unordered_view = (ID3D11UnorderedAccessView*)view;
		switch(shader_type) {
		case L'p':
			// XXX: Not clear if this will unbind other UAVs or not?
			// TODO: Allow pUAVInitialCounts to optionally be set
			mOrigContext1->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
				NULL, NULL, slot, 1, &unordered_view, &uav_counter);
			return;
		case L'c':
			// TODO: Allow pUAVInitialCounts to optionally be set
			mOrigContext1->CSSetUnorderedAccessViews(slot, 1, &unordered_view, &uav_counter);
			return;
		default:
			// Should not happen
			return;
		}
		break;

	case ResourceCopyTargetType::CUSTOM_RESOURCE:
		custom_resource->stride = stride;
		custom_resource->offset = offset;
		custom_resource->format = format;
		custom_resource->buf_size = buf_size;


		if (res == NULL && view == NULL) {
			// Optimisation to allow the resource to be set to null
			// without throwing away the cache so we don't
			// endlessly create & destroy temporary resources.
			custom_resource->is_null = true;
			return;
		}
		custom_resource->is_null = false;

		// If we are passed our own resource (might happen if the
		// resource is used directly in the run() function, or if
		// someone assigned a resource to itself), don't needlessly
		// AddRef() and Release(), and definitely don't Release()
		// before AddRef()
		if (custom_resource->view != view) {
			if (custom_resource->view)
				custom_resource->view->Release();
			custom_resource->view = view;
			if (custom_resource->view)
				custom_resource->view->AddRef();
		}

		if (custom_resource->resource != res) {
			if (custom_resource->resource)
				custom_resource->resource->Release();
			custom_resource->resource = res;
			custom_resource->device = state->mOrigDevice1;
			if (custom_resource->resource)
				custom_resource->resource->AddRef();
		}
		break;

	case ResourceCopyTargetType::THIS_RESOURCE:
		if (state->this_target)
			return state->this_target->SetResource(state, res, view, stride, offset, format, buf_size);

		if (state->resource) {
			if (*state->resource)
				(*state->resource)->Release();
			*state->resource = res;
			break;
		}

		COMMAND_LIST_LOG(state, "  \"this\" target cannot be set outside of a checktextureoverride or indirect draw call context\n");
		break;

	case ResourceCopyTargetType::STEREO_PARAMS:
	case ResourceCopyTargetType::INI_PARAMS:
	case ResourceCopyTargetType::SWAP_CHAIN:
	case ResourceCopyTargetType::FAKE_SWAP_CHAIN:
	case ResourceCopyTargetType::CPU:
		// Only way we could "set" a resource to the (fake) back buffer is by
		// copying to it. Might implement overwrites later, but no
		// pressing need. To write something to the back buffer, assign
		// it as a render target instead.
		//
		// We can't set values on the CPU directly from here, since the
		// values won't have finished transferring yet. These will be
		// set from elsewhere.
		break;
	}
}

D3D11_BIND_FLAG ResourceCopyTarget::BindFlags(CommandListState *state, D3D11_RESOURCE_MISC_FLAG *misc_flags)
{
	switch(type) {
		case ResourceCopyTargetType::CONSTANT_BUFFER:
			return D3D11_BIND_CONSTANT_BUFFER;
		case ResourceCopyTargetType::SHADER_RESOURCE:
			return D3D11_BIND_SHADER_RESOURCE;
		case ResourceCopyTargetType::VERTEX_BUFFER:
			return D3D11_BIND_VERTEX_BUFFER;
		case ResourceCopyTargetType::INDEX_BUFFER:
			return D3D11_BIND_INDEX_BUFFER;
		case ResourceCopyTargetType::STREAM_OUTPUT:
			return D3D11_BIND_STREAM_OUTPUT;
		case ResourceCopyTargetType::RENDER_TARGET:
			return D3D11_BIND_RENDER_TARGET;
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			return D3D11_BIND_DEPTH_STENCIL;
		case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
			return D3D11_BIND_UNORDERED_ACCESS;
		case ResourceCopyTargetType::CUSTOM_RESOURCE:
			if (misc_flags)
				*misc_flags = custom_resource->misc_flags;
			return custom_resource->bind_flags;
		case ResourceCopyTargetType::THIS_RESOURCE:
			if (state) {
				if (state->this_target)
					return state->this_target->BindFlags(state);

				if (state->call_info && state->call_info->indirect_buffer) {
					if (misc_flags)
						*misc_flags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
					// No bind flags required, but a common scenario would be copying
					// from a resource with D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
					// that will need to be cleared out if we have neither SRV or UAV
					// set, which is handled in RecreateCompatibleBuffer()
					return (D3D11_BIND_FLAG)0;
				}
			}
			// Bind flags are unknown since this cannot be resolved
			// until runtime:
			return (D3D11_BIND_FLAG)0;
		case ResourceCopyTargetType::STEREO_PARAMS:
		case ResourceCopyTargetType::INI_PARAMS:
		case ResourceCopyTargetType::SWAP_CHAIN:
		case ResourceCopyTargetType::CPU:
			// N/A since swap chain can't be set as a destination
			return (D3D11_BIND_FLAG)0;
	}

	// Shouldn't happen. No return value makes sense, so raise an exception
	throw(std::range_error("Bad 3DMigoto ResourceCopyTarget"));
}

void ResourceCopyTarget::FindTextureOverrides(CommandListState *state, bool *resource_found, TextureOverrideMatches *matches)
{
	TextureOverrideMap::iterator i;
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	uint32_t hash = 0;

	resource = GetResource(state, &view, NULL, NULL, NULL, NULL);

	if (resource_found)
		*resource_found = !!resource;

	if (!resource)
		return;

	find_texture_overrides_for_resource(resource, matches, state->call_info);

	//COMMAND_LIST_LOG(state, "  found texture hash = %08llx\n", hash);

	resource->Release();
	if (view)
		view->Release();
}

static bool IsCoersionToStructuredBufferRequired(ID3D11View *view, UINT stride,
		UINT offset, DXGI_FORMAT format, D3D11_BIND_FLAG bind_flags)
{
	// If we are copying a vertex buffer into a shader resource we need to
	// convert it into a structured buffer, which requires a flag set when
	// creating the new resource as well as changes in the view.
	//
	// This function tries to detect this situation without explicitly
	// checking that the source was a vertex buffer - that way, similar
	// situations should work as well, such as when using an intermediate
	// resource.

	// If we are copying from a resource that had a view we will use it's
	// description to work out what we need to do (or we will, once I write
	// that code)
	if (view)
		return false;

	// If we know the format there's no need to be structured
	if (format != DXGI_FORMAT_UNKNOWN)
		return false;

	// We need to know the stride to be structured:
	if (stride == 0)
		return false;

	// Structured buffers only make sense for certain views:
	return !!(bind_flags & (D3D11_BIND_SHADER_RESOURCE |
			D3D11_BIND_RENDER_TARGET |
			D3D11_BIND_DEPTH_STENCIL |
			D3D11_BIND_UNORDERED_ACCESS));
}

static ID3D11Buffer *RecreateCompatibleBuffer(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ID3D11Buffer *src_resource,
		ID3D11Buffer *dst_resource,
		ResourcePool *resource_pool,
		ID3D11View *src_view,
		D3D11_BIND_FLAG bind_flags,
		D3D11_RESOURCE_MISC_FLAG misc_flags,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT *buf_dst_size)
{
	D3D11_BUFFER_DESC new_desc;
	ID3D11Buffer *buffer = NULL;
	UINT dst_size;

	src_resource->GetDesc(&new_desc);
	new_desc.BindFlags = bind_flags;
	// We reuse the misc flags from the source, which has worked fairly
	// well for the most part with maybe one or two exceptions. We can
	// always clear incompatible flags and allow an override if necessary:
	new_desc.MiscFlags = new_desc.MiscFlags | misc_flags;

	// Raw view misc flag can only be used with SRVs or UAVs
	if (!(new_desc.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)))
		new_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	if (dst && dst->type == ResourceCopyTargetType::CPU) {
		new_desc.Usage = D3D11_USAGE_STAGING;
		new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	} else {
		new_desc.Usage = D3D11_USAGE_DEFAULT;
		new_desc.CPUAccessFlags = 0;
	}

	if (bind_flags & D3D11_BIND_CONSTANT_BUFFER) {
		// Constant buffers have additional limitations. The size must
		// be a multiple of 16, so round up if necessary, and it cannot
		// be larger than 4096 x 4 component x 4 byte constants.
		dst_size = (new_desc.ByteWidth + 15) & ~0xf;
		dst_size = min(dst_size, D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);

		// Constant buffers cannot be structured, so clear that flag:
		new_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		// XXX: Should we clear StructureByteStride? Seems to work ok
		// without clearing that.

		// If the size of the new resource doesn't match the old or
		// there is an offset we will have to perform a region copy
		// instead of a regular copy:
		if (offset || dst_size != new_desc.ByteWidth) {
			// It might be temping to take the offset into account
			// here and make the buffer only as large as it need to
			// be, but it's possible that the source offset might
			// change much more often than the source buffer (just
			// a guess), which could potentially lead us to
			// constantly recreating the destination buffer.

			// Note down the size of the source and destination:
			*buf_dst_size = dst_size;
			new_desc.ByteWidth = dst_size;
		}
	} else if (IsCoersionToStructuredBufferRequired(src_view, stride, offset, format, bind_flags)) {
		new_desc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		new_desc.StructureByteStride = stride;

		// A structured buffer needs to be a multiple of it's stride,
		// which may not be the case if we're converting a buffer to
		// one. Round it down:
		dst_size = new_desc.ByteWidth / stride * stride;
		// For now always using the region copy if there's an offset.
		// We might not need to do that if the offset is aligned to the
		// stride (although we would need to recreate the view every
		// time it changed), but for now it seems safest to use the
		// region copy method whenever there is an offset:
		if (offset || dst_size != new_desc.ByteWidth) {
			*buf_dst_size = dst_size;
			new_desc.ByteWidth = dst_size;
		}
	} else if (!src_view && offset) {
		// No source view but we do have an offset - use the region
		// copy to knock out the offset. We can probably assume the
		// original resource met all the size and alignment
		// constraints, so we shouldn't need to resize it.
		*buf_dst_size = new_desc.ByteWidth;
	}

	if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		dst->custom_resource->OverrideBufferDesc(&new_desc);

	return GetResourceFromPool<ID3D11Buffer, D3D11_BUFFER_DESC, &ID3D11Device::CreateBuffer>
		(ini_line, src_resource, dst_resource, resource_pool, state, &new_desc);
}

static DXGI_FORMAT MakeTypeless(DXGI_FORMAT fmt)
{
	switch(fmt)
	{
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return DXGI_FORMAT_R32G32B32A32_TYPELESS;

		case DXGI_FORMAT_R32G32B32_FLOAT:
		case DXGI_FORMAT_R32G32B32_UINT:
		case DXGI_FORMAT_R32G32B32_SINT:
			return DXGI_FORMAT_R32G32B32_TYPELESS;

		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
			return DXGI_FORMAT_R16G16B16A16_TYPELESS;

		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return DXGI_FORMAT_R32G32_TYPELESS;

		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_R32G8X24_TYPELESS;

		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UINT:
			return DXGI_FORMAT_R10G10B10A2_TYPELESS;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SNORM:
		case DXGI_FORMAT_R8G8B8A8_SINT:
			return DXGI_FORMAT_R8G8B8A8_TYPELESS;

		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UNORM:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16G16_SNORM:
		case DXGI_FORMAT_R16G16_SINT:
			return DXGI_FORMAT_R16G16_TYPELESS;

		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
			return DXGI_FORMAT_R32_TYPELESS;

		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_R24G8_TYPELESS;

		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
			return DXGI_FORMAT_R8G8_TYPELESS;

		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
			return DXGI_FORMAT_R16_TYPELESS;

		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
			return DXGI_FORMAT_R8_TYPELESS;

		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			return DXGI_FORMAT_BC1_TYPELESS;

		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			return DXGI_FORMAT_BC2_TYPELESS;

		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			return DXGI_FORMAT_BC3_TYPELESS;

		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return DXGI_FORMAT_BC4_TYPELESS;

		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
			return DXGI_FORMAT_BC5_TYPELESS;

		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_TYPELESS;

		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8X8_TYPELESS;

		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
			return DXGI_FORMAT_BC6H_TYPELESS;

		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return DXGI_FORMAT_BC7_TYPELESS;

		case DXGI_FORMAT_R11G11B10_FLOAT:
		default:
			return fmt;
	}
}

static DXGI_FORMAT MakeDSVFormat(DXGI_FORMAT fmt)
{
	switch(fmt)
	{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return DXGI_FORMAT_D32_FLOAT;

		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;

		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
			return DXGI_FORMAT_D16_UNORM;

		default:
			return EnsureNotTypeless(fmt);
	}
}

static DXGI_FORMAT MakeNonDSVFormat(DXGI_FORMAT fmt)
{
	// TODO: Add a keyword to return the stencil side of a combined
	// depth/stencil resource instead of the depth side
	switch(fmt)
	{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return DXGI_FORMAT_R32_FLOAT;

		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_UNORM:
			return DXGI_FORMAT_R16_UNORM;

		default:
			return EnsureNotTypeless(fmt);
	}
}

// MSAA resolving only makes sense for Texture2D types, and the SampleDesc
// entry only exists in those. Use template specialisation so we don't have to
// duplicate the entire RecreateCompatibleTexture() routine for such a small
// difference.
template <typename DescType>
static void Texture2DDescResolveMSAA(DescType *desc) {}
template <>
static void Texture2DDescResolveMSAA(D3D11_TEXTURE2D_DESC *desc)
{
	desc->SampleDesc.Count = 1;
	desc->SampleDesc.Quality = 0;
}

template <typename ResourceType,
	 typename DescType,
	HRESULT (__stdcall ID3D11Device::*CreateTexture)(THIS_
	      const DescType *pDesc,
	      const D3D11_SUBRESOURCE_DATA *pInitialData,
	      ResourceType **ppTexture)
	>
static ResourceType* RecreateCompatibleTexture(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ResourceType *src_resource,
		ResourceType *dst_resource,
		ResourcePool *resource_pool,
		D3D11_BIND_FLAG bind_flags,
		CommandListState *state,
		StereoHandle mStereoHandle,
		ResourceCopyOptions options)
{
	DescType new_desc;

	src_resource->GetDesc(&new_desc);
	new_desc.BindFlags = bind_flags;

	if (dst && dst->type == ResourceCopyTargetType::CPU) {
		new_desc.Usage = D3D11_USAGE_STAGING;
		new_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	} else {
		new_desc.Usage = D3D11_USAGE_DEFAULT;
		new_desc.CPUAccessFlags = 0;
	}

	// New strategy - we make the new resources typeless whenever possible
	// and will fill the type back in in the view instead. This gives us
	// more flexibility with depth/stencil formats which need different
	// types depending on where they are bound in the pipeline. This also
	// helps with certain MSAA resources that may not be possible to create
	// if we change the type to a R*X* format.
	new_desc.Format = MakeTypeless(new_desc.Format);

	if (options & ResourceCopyOptions::STEREO2MONO)
		new_desc.Width *= 2;

	// TODO: reverse_blit might need to imply resolve_msaa:
	if (options & ResourceCopyOptions::RESOLVE_MSAA)
		Texture2DDescResolveMSAA(&new_desc);

	// XXX: Any changes needed in new_desc.MiscFlags?
	//
	// D3D11_RESOURCE_MISC_GENERATE_MIPS requires specific bind flags (both
	// shader resource AND render target must be set) and might prevent us
	// from creating the resource otherwise. Since we don't need to
	// generate mip-maps just clear it out:
	new_desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;

	if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		dst->custom_resource->OverrideTexDesc(&new_desc);

	return GetResourceFromPool<ResourceType, DescType, CreateTexture>
		(ini_line, src_resource, dst_resource, resource_pool, state, &new_desc);
}

static void RecreateCompatibleResource(
		wstring *ini_line,
		ResourceCopyTarget *dst, // May be NULL
		ID3D11Resource *src_resource,
		ID3D11Resource **dst_resource,
		ResourcePool *resource_pool,
		ID3D11View *src_view,
		ID3D11View **dst_view,
		CommandListState *state,
		StereoHandle mStereoHandle,
		ResourceCopyOptions options,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT *buf_dst_size)
{
	NVAPI_STEREO_SURFACECREATEMODE orig_mode = NVAPI_STEREO_SURFACECREATEMODE_AUTO;
	D3D11_RESOURCE_DIMENSION src_dimension;
	D3D11_BIND_FLAG bind_flags = (D3D11_BIND_FLAG)0;
	D3D11_RESOURCE_MISC_FLAG misc_flags = (D3D11_RESOURCE_MISC_FLAG)0;
	ID3D11Resource *res = NULL;
	bool restore_create_mode = false;

	if (dst)
		bind_flags = dst->BindFlags(state, &misc_flags);

	LockResourceCreationMode();

	if (options & ResourceCopyOptions::CREATEMODE_MASK) {
		Profiling::NvAPI_Stereo_GetSurfaceCreationMode(mStereoHandle, &orig_mode);
		restore_create_mode = true;

		// STEREO2MONO will force the final destination to mono since
		// it is in the CREATEMODE_MASK, but is not STEREO. It also
		// creates an additional intermediate resource that will be
		// forced to STEREO.

		if (options & ResourceCopyOptions::STEREO) {
			Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);
		} else {
			Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle,
					NVAPI_STEREO_SURFACECREATEMODE_FORCEMONO);
		}
	} else if (dst && dst->type == ResourceCopyTargetType::CUSTOM_RESOURCE) {
		restore_create_mode = dst->custom_resource->OverrideSurfaceCreationMode(mStereoHandle, &orig_mode);
	}

	src_resource->GetType(&src_dimension);
	switch (src_dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			res = RecreateCompatibleBuffer(ini_line, dst, (ID3D11Buffer*)src_resource, (ID3D11Buffer*)*dst_resource,
				resource_pool, src_view, bind_flags, misc_flags, state, stride, offset, format, buf_dst_size);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			res = RecreateCompatibleTexture<ID3D11Texture1D, D3D11_TEXTURE1D_DESC, &ID3D11Device::CreateTexture1D>
				(ini_line, dst, (ID3D11Texture1D*)src_resource, (ID3D11Texture1D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			res = RecreateCompatibleTexture<ID3D11Texture2D, D3D11_TEXTURE2D_DESC, &ID3D11Device::CreateTexture2D>
				(ini_line, dst, (ID3D11Texture2D*)src_resource, (ID3D11Texture2D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			res = RecreateCompatibleTexture<ID3D11Texture3D, D3D11_TEXTURE3D_DESC, &ID3D11Device::CreateTexture3D>
				(ini_line, dst, (ID3D11Texture3D*)src_resource, (ID3D11Texture3D*)*dst_resource, resource_pool,
				 bind_flags, state, mStereoHandle, options);
			break;
	}

	if (restore_create_mode)
		Profiling::NvAPI_Stereo_SetSurfaceCreationMode(mStereoHandle, orig_mode);

	UnlockResourceCreationMode();

	if (res) {
		if (*dst_resource)
			(*dst_resource)->Release();
		if (dst_view && *dst_view)
			(*dst_view)->Release();

		*dst_resource = res;
		if (dst_view)
			*dst_view = NULL;
	}
}

template <typename DescType>
static void FillOutBufferDescCommon(DescType *desc, UINT stride,
		UINT offset, UINT buf_src_size)
{
	// The documentation on the buffer part of the description is
	// misleading.
	//
	// There are two unions with two possible parameters each which
	// are documented in MSDN, but DX11 never uses ElementWidth
	// (which is determined by either the format, or buffer's
	// StructureByteStride), only NumElements.
	//
	// My reading of FirstElement/ElementOffset sound like they are
	// the same thing, but one is in bytes and the other is in
	// elements - only the names seem backwards compared to the
	// description in the documentation. Research suggests DX11
	// only uses multiples of the element size (since it's a union,
	// it shouldn't matter which name we use).
	//
	// XXX: At the moment we are relying on the region copy to have
	// knocked out the offset for us. We could alternatively do it
	// here (and the below should work), but we would need to
	// create a new view every time the offset changes.
	//
	// Possible TODO: Handle vertex/index buffers with "first vertex/index"
	// here? These can now be accessed via command list expression and
	// passed through ini params, so a fix can handle them that way, and
	// any change here would need careful consideration as to backwards
	// compatibility.
	if (stride) {
		desc->FirstElement = offset / stride;
		desc->NumElements = (buf_src_size - offset) / stride;
	} else {
		desc->FirstElement = 0;
		desc->NumElements = 1;
	}
}

static bool requires_raw_view(ID3D11Buffer *buf, DXGI_FORMAT format)
{
	D3D11_BUFFER_DESC buf_desc;

	buf->GetDesc(&buf_desc);
	if (!(buf_desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS))
		return false;

	// The buffer allows raw views, but that doesn't forbid other access.
	// However, other non-structured access (structured cannot have the
	// above flag) requires us to know the data format, so if we don't know
	// the data type than raw access is the only remaining option:
	switch (format) {
		case DXGI_FORMAT_UNKNOWN:
		case DXGI_FORMAT_R32_TYPELESS:
			return true;
	}

	return false;
}

static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutBufferDesc(ID3D11Buffer *buf,
		D3D11_SHADER_RESOURCE_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size, ResourceCopyOptions options)
{
	if (options & ResourceCopyOptions::RAW_VIEW || requires_raw_view(buf, desc->Format)) {
               desc->ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
               desc->BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
	       // Although not specified in MSDN, raw views in SRVs appear to
	       // require the R32_TYPELESS format (MSDN does specify this
	       // requirement for raw UAVs) and fail if any other format is
	       // specified. The only curiosity here is that they are described
	       // as being accessible in 1-4 channels, seeming to imply that
	       // formats like R32G32B32A32_TYPELESS should also work, but they
	       // don't:
	       desc->Format = DXGI_FORMAT_R32_TYPELESS;
	       stride = 4;
	       FillOutBufferDescCommon<D3D11_BUFFEREX_SRV>(&desc->BufferEx, stride, offset, buf_src_size);
	} else {
		desc->ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		FillOutBufferDescCommon<D3D11_BUFFER_SRV>(&desc->Buffer, stride, offset, buf_src_size);
	}
	return desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutBufferDesc(ID3D11Buffer *buf,
		D3D11_RENDER_TARGET_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size, ResourceCopyOptions options)
{
	desc->ViewDimension = D3D11_RTV_DIMENSION_BUFFER;

	FillOutBufferDescCommon<D3D11_BUFFER_RTV>(&desc->Buffer, stride, offset, buf_src_size);
	return desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutBufferDesc(ID3D11Buffer *buf,
		D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size, ResourceCopyOptions options)
{
	desc->ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	desc->Buffer.Flags = 0;

	if (options & ResourceCopyOptions::RAW_VIEW || requires_raw_view(buf, desc->Format)) {
		desc->Buffer.Flags |= D3D11_BUFFER_UAV_FLAG_RAW;
		desc->Format = DXGI_FORMAT_R32_TYPELESS;
		stride = 4;
	}
	// TODO Support buffer UAV flags for append and counter buffers.

	FillOutBufferDescCommon<D3D11_BUFFER_UAV>(&desc->Buffer, stride, offset, buf_src_size);
	return desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutBufferDesc(ID3D11Buffer *buf,
		D3D11_DEPTH_STENCIL_VIEW_DESC *desc, UINT stride,
		UINT offset, UINT buf_src_size, ResourceCopyOptions options)
{
	// Depth views don't support buffers:
	return NULL;
}


// This is a hell of a lot of duplicated code, mostly thanks to DX using
// different names for the same thing in a slightly different type, and pretty
// much all this is only needed for depth/stencil format conversions. It would
// be nice to refactor this somehow. TODO: For now we are creating a view of
// the entire resource, but it would make sense to use information from the
// source view if available instead.
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex1DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MostDetailedMip = 0;
		view_desc->Texture1D.MipLevels = -1;
	} else {
		view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MostDetailedMip = 0;
		view_desc->Texture1DArray.MipLevels = -1;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex1DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex1DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex1DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		D3D11_TEXTURE1D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
		view_desc->Texture1D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
		view_desc->Texture1DArray.MipSlice = 0;
		view_desc->Texture1DArray.FirstArraySlice = 0;
		view_desc->Texture1DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex2DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
			view_desc->TextureCube.MostDetailedMip = 0;
			view_desc->TextureCube.MipLevels = -1;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
			view_desc->TextureCubeArray.MostDetailedMip = 0;
			view_desc->TextureCubeArray.MipLevels = -1;
			view_desc->TextureCubeArray.First2DArrayFace = 0; // FIXME: Get from original view
			view_desc->TextureCubeArray.NumCubes = resource_desc->ArraySize / 6;
		}
	} else if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MostDetailedMip = 0;
			view_desc->Texture2D.MipLevels = -1;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MostDetailedMip = 0;
			view_desc->Texture2DArray.MipLevels = -1;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex2DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MipSlice = 0;
		} else {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MipSlice = 0;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex2DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeDSVFormat(format);
	view_desc->Flags = 0; // TODO: Fill in from old view, and add keyword to override

	if (resource_desc->SampleDesc.Count == 1) {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			view_desc->Texture2D.MipSlice = 0;
		} else {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			view_desc->Texture2DArray.MipSlice = 0;
			view_desc->Texture2DArray.FirstArraySlice = 0;
			view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
		}
	} else {
		if (resource_desc->ArraySize == 1) {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
		} else {
			view_desc->ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
			view_desc->Texture2DMSArray.FirstArraySlice = 0;
			view_desc->Texture2DMSArray.ArraySize = resource_desc->ArraySize;
		}
	}

	return view_desc;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex2DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		D3D11_TEXTURE2D_DESC *resource_desc, DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	if (resource_desc->ArraySize == 1) {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		view_desc->Texture2D.MipSlice = 0;
	} else {
		view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		view_desc->Texture2DArray.MipSlice = 0;
		view_desc->Texture2DArray.FirstArraySlice = 0;
		view_desc->Texture2DArray.ArraySize = resource_desc->ArraySize;
	}

	return view_desc;
}
static D3D11_SHADER_RESOURCE_VIEW_DESC* FillOutTex3DDesc(
		D3D11_SHADER_RESOURCE_VIEW_DESC *view_desc,
		DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MostDetailedMip = 0;
	view_desc->Texture3D.MipLevels = -1;

	return view_desc;
}
static D3D11_RENDER_TARGET_VIEW_DESC* FillOutTex3DDesc(
		D3D11_RENDER_TARGET_VIEW_DESC *view_desc,
		DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MipSlice = 0;
	view_desc->Texture3D.FirstWSlice = 0;
	view_desc->Texture3D.WSize = -1;

	return view_desc;
}
static D3D11_DEPTH_STENCIL_VIEW_DESC* FillOutTex3DDesc(
		D3D11_DEPTH_STENCIL_VIEW_DESC *view_desc,
		DXGI_FORMAT format)
{
	// DSV cannot be a Texture3D

	return NULL;
}
static D3D11_UNORDERED_ACCESS_VIEW_DESC* FillOutTex3DDesc(
		D3D11_UNORDERED_ACCESS_VIEW_DESC *view_desc,
		DXGI_FORMAT format)
{
	view_desc->Format = MakeNonDSVFormat(format);

	view_desc->ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	view_desc->Texture3D.MipSlice = 0;
	view_desc->Texture3D.FirstWSlice = 0;
	view_desc->Texture3D.WSize = -1;

	return view_desc;
}


template <typename ViewType,
	 typename DescType,
	 HRESULT (__stdcall ID3D11Device::*CreateView)(THIS_
			 ID3D11Resource *pResource,
			 const DescType *pDesc,
			 ViewType **ppView)
	>
static ID3D11View* _CreateCompatibleView(
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size,
		ResourceCopyOptions options)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Buffer *buf;
	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	ViewType *view = NULL;
	DescType view_desc, *pDesc = NULL;
	HRESULT hr;

	resource->GetType(&dimension);
	switch(dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			// In the case of a buffer type resource we must specify the
			// description as DirectX doesn't have enough information from the
			// buffer alone to create a view.

			view_desc.Format = format;

			buf = (ID3D11Buffer*)resource;
			pDesc = FillOutBufferDesc(buf, &view_desc, stride, offset, buf_src_size, options);

			// This should already handle things like:
			// - Copying a vertex buffer to a SRV or constant buffer
			// - Copying an index buffer to a SRV
			// - Copying structured buffers
			// - Copying regular buffers

			// TODO: Support UAV flags like append/consume and SRV BufferEx views
			break;

		// We now also fill out the view description for textures as
		// well. We used to create fully typed resources and leave this
		// up to DX, but there were some situations where that would
		// not work (depth buffers need different types depending on
		// where they are bound, some MSAA resources could not be
		// created), so we now create typeless resources and therefore
		// have to fill out the view description to set the type. We
		// could potentially do this for only the cases where we need
		// (i.e. depth buffer formats), but I want to do this for
		// everything because it's so damn overly complex that typos
		// are ensured so this way it will at least get more exposure
		// and I can find the bugs sooner:
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			pDesc = FillOutTex1DDesc(&view_desc, &tex1d_desc, format);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			pDesc = FillOutTex2DDesc(&view_desc, &tex2d_desc, format);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			pDesc = FillOutTex3DDesc(&view_desc, format);
			break;
	}

	hr = (state->mOrigDevice1->*CreateView)(resource, pDesc, &view);
	if (FAILED(hr)) {
		LogInfo("Resource copy CreateCompatibleView failed: %x\n", hr);
		if (pDesc)
			LogViewDesc(pDesc);
		LogResourceDesc(resource);
		return NULL;
	}

	if (pDesc)
		LogDebugViewDesc(pDesc);

	return view;
}

static ID3D11View* CreateCompatibleView(
		ResourceCopyTarget *dst,
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size,
		ResourceCopyOptions options)
{
	switch (dst->type) {
		case ResourceCopyTargetType::SHADER_RESOURCE:
			return _CreateCompatibleView<ID3D11ShaderResourceView,
			       D3D11_SHADER_RESOURCE_VIEW_DESC,
			       &ID3D11Device::CreateShaderResourceView>
				       (resource, state, stride, offset, format, buf_src_size, options);
		case ResourceCopyTargetType::RENDER_TARGET:
			return _CreateCompatibleView<ID3D11RenderTargetView,
			       D3D11_RENDER_TARGET_VIEW_DESC,
			       &ID3D11Device::CreateRenderTargetView>
				       (resource, state, stride, offset, format, buf_src_size, options);
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			return _CreateCompatibleView<ID3D11DepthStencilView,
			       D3D11_DEPTH_STENCIL_VIEW_DESC,
			       &ID3D11Device::CreateDepthStencilView>
				       (resource, state, stride, offset, format, buf_src_size, options);
		case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
			return _CreateCompatibleView<ID3D11UnorderedAccessView,
			       D3D11_UNORDERED_ACCESS_VIEW_DESC,
			       &ID3D11Device::CreateUnorderedAccessView>
				       (resource, state, stride, offset, format, buf_src_size, options);
		case ResourceCopyTargetType::THIS_RESOURCE:
			if (state->this_target)
				return CreateCompatibleView(state->this_target, resource, state, stride, offset, format, buf_src_size, options);
			break;
	}
	return NULL;
}

static void SetViewportFromResource(CommandListState *state, ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Texture1D *tex1d;
	ID3D11Texture2D *tex2d;
	ID3D11Texture3D *tex3d;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;
	D3D11_VIEWPORT viewport = {0, 0, 0, 0, D3D11_MIN_DEPTH, D3D11_MAX_DEPTH};

	// TODO: Could handle mip-maps from a view like the CD3D11_VIEWPORT
	// constructor, but we aren't using them elsewhere so don't care yet.
	resource->GetType(&dimension);
	switch(dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			// TODO: Width = NumElements
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			viewport.Width = (float)tex1d_desc.Width;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			viewport.Width = (float)tex2d_desc.Width;
			viewport.Height = (float)tex2d_desc.Height;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)resource;
			tex3d->GetDesc(&tex3d_desc);
			viewport.Width = (float)tex3d_desc.Width;
			viewport.Height = (float)tex3d_desc.Height;
	}

	state->mOrigContext1->RSSetViewports(1, &viewport);
}

ResourceCopyOperation::ResourceCopyOperation() :
	options(ResourceCopyOptions::INVALID),
	cached_resource(NULL),
	cached_view(NULL),
	stereo2mono_intermediate(NULL)
{}

ResourceCopyOperation::~ResourceCopyOperation()
{
	if (cached_resource)
		cached_resource->Release();

	if (cached_view)
		cached_view->Release();
}

ResourceStagingOperation::ResourceStagingOperation()
{
	dst.type = ResourceCopyTargetType::CPU;
	options = ResourceCopyOptions::COPY;
	staging = false;
	ini_line = L"  Beginning transfer to CPU...";
}

HRESULT ResourceStagingOperation::map(CommandListState *state, D3D11_MAPPED_SUBRESOURCE *mapping)
{
	if (!cached_resource)
		return E_FAIL;

	return state->mOrigContext1->Map(cached_resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, mapping);
}

void ResourceStagingOperation::unmap(CommandListState *state)
{
	if (cached_resource)
		state->mOrigContext1->Unmap(cached_resource, 0);
}

static void ResolveMSAA(ID3D11Resource *dst_resource, ID3D11Resource *src_resource, CommandListState *state)
{
	UINT item, level, index, support;
	D3D11_RESOURCE_DIMENSION dst_dimension;
	ID3D11Texture2D *src, *dst;
	D3D11_TEXTURE2D_DESC desc;
	DXGI_FORMAT fmt;
	HRESULT hr;

	dst_resource->GetType(&dst_dimension);
	if (dst_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		return;

	src = (ID3D11Texture2D*)src_resource;
	dst = (ID3D11Texture2D*)dst_resource;

	dst->GetDesc(&desc);
	fmt = EnsureNotTypeless(desc.Format);

	hr = state->mOrigDevice1->CheckFormatSupport( fmt, &support );
	if (FAILED(hr) || !(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
		// TODO: Implement a fallback using a SM5 shader to resolve it
		LogInfo("Resource copy cannot resolve MSAA format %d\n", fmt);
		return;
	}

	for (item = 0; item < desc.ArraySize; item++) {
		for (level = 0; level < desc.MipLevels; level++) {
			index = D3D11CalcSubresource(level, item, max(desc.MipLevels, 1));
			state->mOrigContext1->ResolveSubresource(dst, index, src, index, fmt);
		}
	}
}

static void ReverseStereoBlit(ID3D11Resource *dst_resource, ID3D11Resource *src_resource, CommandListState *state)
{
	NvAPI_Status nvret;
	D3D11_RESOURCE_DIMENSION src_dimension;
	ID3D11Texture2D *src;
	D3D11_TEXTURE2D_DESC srcDesc;
	UINT item, level, index, width, height;
	D3D11_BOX srcBox;
	int fallbackside, fallback = 0;

	src_resource->GetType(&src_dimension);
	if (src_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		// TODO: I think it should be possible to do this with all
		// resource types (possibly including buffers from the
		// discovery of the stereo parameters in the cb12 slot), but I
		// need to test it and make sure it works first
		LogInfo("Resource copy: Reverse stereo blit not supported on resource type %d\n", src_dimension);
		return;
	}

	src = (ID3D11Texture2D*)src_resource;
	src->GetDesc(&srcDesc);

	// TODO: Resolve MSAA
	// TODO: Use intermediate resource if copying from a texture with depth buffer bind flags

	// If stereo is disabled the reverse stereo blit won't work and we
	// would end up with the destination only updated on the left, which
	// may lead to shaders reading stale or 0 data if they read from the
	// right hand side. Use the fallback path to copy the source to both
	// sides of the destination so that the right side will be up to date:
	fallback = state->mHackerDevice->mParamTextureManager.mActive ? 0 : 1;

	if (!fallback) {
		nvret = Profiling::NvAPI_Stereo_ReverseStereoBlitControl(state->mHackerDevice->mStereoHandle, true);
		if (nvret != NVAPI_OK) {
			LogInfo("Resource copying failed to enable reverse stereo blit\n");
			// Fallback path: Copy 2D resource to both sides of the 2x
			// width destination
			fallback = 1;
		}
	}

	for (fallbackside = 0; fallbackside < 1 + fallback; fallbackside++) {

		// Set the source box as per the nvapi documentation:
		srcBox.left = 0;
		srcBox.top = 0;
		srcBox.front = 0;
		srcBox.right = width = srcDesc.Width;
		srcBox.bottom = height = srcDesc.Height;
		srcBox.back = 1;

		// Perform the reverse stereo blit on all sub-resources and mip-maps:
		for (item = 0; item < srcDesc.ArraySize; item++) {
			for (level = 0; level < srcDesc.MipLevels; level++) {
				index = D3D11CalcSubresource(level, item, max(srcDesc.MipLevels, 1));
				srcBox.right = width >> level;
				srcBox.bottom = height >> level;
				state->mOrigContext1->CopySubresourceRegion(dst_resource, index,
						fallbackside * srcBox.right, 0, 0,
						src, index, &srcBox);
			}
		}
	}

	if (!fallback)
		Profiling::NvAPI_Stereo_ReverseStereoBlitControl(state->mHackerDevice->mStereoHandle, false);
}

static void SpecialCopyBufferRegion(ID3D11Resource *dst_resource,ID3D11Resource *src_resource,
		CommandListState *state, UINT stride, UINT *offset,
		UINT buf_src_size, UINT buf_dst_size)
{
	// We are copying a buffer for use in a constant buffer and the size of
	// the original buffer did not meet the constraints of a constant
	// buffer.
	D3D11_BOX src_box;

	// We want to copy from the offset to the end of the source buffer, but
	// cap it to the destination size to avoid "undefined behaviour". Keep
	// in mind that this is "right", not "size":
	src_box.left = *offset;
	src_box.right = min(buf_src_size, *offset + buf_dst_size);

	if (stride) {
		// If we are copying to a structured resource, the source box
		// must be a multiple of the stride, so round it down:
		src_box.right = (src_box.right - src_box.left) / stride * stride + src_box.left;
	}

	src_box.top = 0;
	src_box.bottom = 1;
	src_box.front = 0;
	src_box.back = 1;

	state->mOrigContext1->CopySubresourceRegion(dst_resource, 0, 0, 0, 0, src_resource, 0, &src_box);

	// We have effectively removed the offset during the region copy, so
	// set it to 0 to make sure nothing will try to use it again elsewhere:
	*offset = 0;
}

static UINT get_resource_bind_flags(ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension;
	ID3D11Buffer *buf = NULL;
	ID3D11Texture1D *tex1d = NULL;
	ID3D11Texture2D *tex2d = NULL;
	ID3D11Texture3D *tex3d = NULL;
	D3D11_BUFFER_DESC buf_desc;
	D3D11_TEXTURE1D_DESC tex1d_desc;
	D3D11_TEXTURE2D_DESC tex2d_desc;
	D3D11_TEXTURE3D_DESC tex3d_desc;

	resource->GetType(&dimension);
	switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			buf = (ID3D11Buffer*)resource;
			buf->GetDesc(&buf_desc);
			return buf_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex1d = (ID3D11Texture1D*)resource;
			tex1d->GetDesc(&tex1d_desc);
			return tex1d_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2d = (ID3D11Texture2D*)resource;
			tex2d->GetDesc(&tex2d_desc);
			return tex2d_desc.BindFlags;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3d = (ID3D11Texture3D*)resource;
			tex3d->GetDesc(&tex3d_desc);
			return tex3d_desc.BindFlags;
	}
	return 0;
}

ID3D11View* ClearViewCommand::create_best_view(
		ID3D11Resource *resource,
		CommandListState *state,
		UINT stride,
		UINT offset,
		DXGI_FORMAT format,
		UINT buf_src_size)
{
	UINT bind_flags;

	// FIXME: If a buffer can only be accessed via a raw view we may need
	// to handle that case here:
	ResourceCopyOptions options = ResourceCopyOptions::INVALID;

	// We didn't get a view, so we will have to create one, but
	// which type? We will guess based on what the user specified
	// and what bind flags the resource has.

	FillInMissingInfo(target.type, resource, NULL, &stride, &offset,
			&buf_src_size, &format);

	// If the user specified "depth" and/or "stencil" they gave us
	// the answer:
	if (clear_depth || clear_stencil) {
		return _CreateCompatibleView<ID3D11DepthStencilView,
		       D3D11_DEPTH_STENCIL_VIEW_DESC,
		       &ID3D11Device::CreateDepthStencilView>
			       (resource, state, stride, offset, format, buf_src_size, options);
	}

	// If the user specified "int" or used a hex string then it
	// must be a UAV and we must be doing an int clear on it:
	if (clear_uav_uint) {
		return _CreateCompatibleView<ID3D11UnorderedAccessView,
		       D3D11_UNORDERED_ACCESS_VIEW_DESC,
		       &ID3D11Device::CreateUnorderedAccessView>
			       (resource, state, stride, offset, format, buf_src_size, options);
	}

	// Otherwise just make whatever view is compatible with the bind flags.
	// Since views may have multiple bind flags let's prioritise the more
	// esoteric DSV and UAV before RTV on the theory that if they are
	// available then we are more likely to want to use their clear
	// methods.
	bind_flags = get_resource_bind_flags(resource);
	if (bind_flags & D3D11_BIND_DEPTH_STENCIL) {
		return _CreateCompatibleView<ID3D11DepthStencilView,
		       D3D11_DEPTH_STENCIL_VIEW_DESC,
		       &ID3D11Device::CreateDepthStencilView>
			       (resource, state, stride, offset, format, buf_src_size, options);
	}
	if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) {
		return _CreateCompatibleView<ID3D11UnorderedAccessView,
		       D3D11_UNORDERED_ACCESS_VIEW_DESC,
		       &ID3D11Device::CreateUnorderedAccessView>
			       (resource, state, stride, offset, format, buf_src_size, options);
	}
	if (bind_flags & D3D11_BIND_RENDER_TARGET) {
		return _CreateCompatibleView<ID3D11RenderTargetView,
		       D3D11_RENDER_TARGET_VIEW_DESC,
		       &ID3D11Device::CreateRenderTargetView>
			       (resource, state, stride, offset, format, buf_src_size, options);
	}
	// TODO: In DX 11.1 there is a generic clear routine, so SRVs might work?
	return NULL;
}

void ClearViewCommand::clear_unknown_view(ID3D11View *view, CommandListState *state)
{
	ID3D11RenderTargetView *rtv = NULL;
	ID3D11DepthStencilView *dsv = NULL;
	ID3D11UnorderedAccessView *uav = NULL;

	// We have a view, but we don't know what kind of view it is. We could
	// infer that from the target type, but in the future CustomResource
	// targets will return a cached view as well (they already have a view
	// today, but if you follow the logic closely you will realise we don't
	// have any code paths that will decide to use it), so to try to future
	// proof this let's use QueryInterface() to see which interfaces the
	// view supports to tell us what kind it is:
	view->QueryInterface(__uuidof(ID3D11RenderTargetView), (void**)&rtv);
	view->QueryInterface(__uuidof(ID3D11DepthStencilView), (void**)&dsv);
	view->QueryInterface(__uuidof(ID3D11UnorderedAccessView), (void**)&uav);

	if (rtv) {
		COMMAND_LIST_LOG(state, "  clearing RTV\n");
		Profiling::views_cleared++;
		state->mOrigContext1->ClearRenderTargetView(rtv, fval);
	}
	if (dsv) {
		D3D11_CLEAR_FLAG flags = (D3D11_CLEAR_FLAG)0;
		COMMAND_LIST_LOG(state, "  clearing DSV\n");
		Profiling::views_cleared++;

		if (!clear_depth && !clear_stencil)
			flags = (D3D11_CLEAR_FLAG)(D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL);
		else if (clear_depth)
			flags = D3D11_CLEAR_DEPTH;
		else if (clear_stencil)
			flags = D3D11_CLEAR_STENCIL;

		state->mOrigContext1->ClearDepthStencilView(dsv, flags, dsv_depth, dsv_stencil);
	}
	if (uav) {
		// We can clear UAVs with either floats or uints, but which
		// should we use? The API call doesn't let us know if it
		// failed, and floats will only work with specific view
		// formats, so we try to predict if the float clear will pass
		// unless the user specificially told us to use the int clear.
		if (clear_uav_uint || !UAVSupportsFloatClear(uav)) {
			COMMAND_LIST_LOG(state, "  clearing UAV (uint)\n");
			state->mOrigContext1->ClearUnorderedAccessViewUint(uav, uval);
		} else {
			COMMAND_LIST_LOG(state, "  clearing UAV (float)\n");
			state->mOrigContext1->ClearUnorderedAccessViewFloat(uav, fval);
		}
		Profiling::views_cleared++;
	}

	if (rtv)
		rtv->Release();
	if (dsv)
		dsv->Release();
	if (uav)
		uav->Release();
}

void ClearViewCommand::run(CommandListState *state)
{
	ID3D11Resource *resource = NULL;
	ID3D11View *view = NULL;
	UINT stride = 0;
	UINT offset = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT buf_src_size = 0;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	resource = target.GetResource(state, &view, &stride, &offset, &format, &buf_src_size);
	if (!resource) {
		COMMAND_LIST_LOG(state, "  No resource to clear\n");
		return;
	}

	if (!view)
		view = create_best_view(resource, state, stride, offset, format, buf_src_size);

	if (view)
		clear_unknown_view(view, state);
	else
		COMMAND_LIST_LOG(state, "  No view and unable to create view to clear resource\n");

	if (resource)
		resource->Release();
	if (view)
		view->Release();
}


static bool ViewMatchesResource(ID3D11View *view, ID3D11Resource *resource)
{
	ID3D11Resource *tmp_resource = NULL;

	view->GetResource(&tmp_resource);
	if (!tmp_resource)
		return false;
	tmp_resource->Release();

	return (tmp_resource == resource);
}

// Returns the equivelent target type of built in targets with pre-existing
// views, so that we don't go and create a view cache when we already have one
// we could use directly:
static ResourceCopyTargetType EquivTarget(ResourceCopyTargetType type)
{
	switch(type) {
		case ResourceCopyTargetType::STEREO_PARAMS:
		case ResourceCopyTargetType::INI_PARAMS:
		case ResourceCopyTargetType::CURSOR_MASK:
		case ResourceCopyTargetType::CURSOR_COLOR:
			return ResourceCopyTargetType::SHADER_RESOURCE;
	}
	return type;
}

void ResourceCopyOperation::run(CommandListState *state)
{
	HackerDevice *mHackerDevice = state->mHackerDevice;
	HackerContext *mHackerContext = state->mHackerContext;
	ID3D11DeviceContext *mOrigContext1 = state->mOrigContext1;
	ID3D11Resource *src_resource = NULL;
	ID3D11Resource *dst_resource = NULL;
	ID3D11Resource **pp_cached_resource = &cached_resource;
	ID3D11Device **pp_cached_device = NULL;
	ResourcePool *p_resource_pool = &resource_pool;
	ID3D11View *src_view = NULL;
	ID3D11View *dst_view = NULL;
	ID3D11View **pp_cached_view = &cached_view;
	UINT stride = 0;
	UINT offset = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT buf_src_size = 0, buf_dst_size = 0;

	COMMAND_LIST_LOG(state, "%S\n", ini_line.c_str());

	if (src.type == ResourceCopyTargetType::EMPTY) {
		dst.SetResource(state, NULL, NULL, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
		return;
	}

	src_resource = src.GetResource(state, &src_view, &stride, &offset, &format, &buf_src_size,
			((options & ResourceCopyOptions::REFERENCE) ? &dst : NULL));
	if (!src_resource) {
		COMMAND_LIST_LOG(state, "  Copy source was NULL\n");
		if (!(options & ResourceCopyOptions::UNLESS_NULL)) {
			// Still set destination to NULL - if we are copying a
			// resource we generally expect it to be there, and
			// this will make errors more obvious if we copy
			// something that doesn't exist. This behaviour can be
			// overridden with the unless_null keyword.
			dst.SetResource(state, NULL, NULL, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
		}
		return;
	}

	if (dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE) {
		// If we're copying to a custom resource, use the resource &
		// view in the CustomResource directly as the cache instead of
		// the cache in the ResourceCopyOperation. This will reduce the
		// number of extra resources we have floating around if copying
		// something to a single custom resource from multiple shaders.
		pp_cached_resource = &dst.custom_resource->resource;
		pp_cached_device = &dst.custom_resource->device;
		p_resource_pool = &dst.custom_resource->resource_pool;
		pp_cached_view = &dst.custom_resource->view;

		if (dst.custom_resource->max_copies_per_frame) {
			if (dst.custom_resource->frame_no != G->frame_no) {
				dst.custom_resource->frame_no = G->frame_no;
				dst.custom_resource->copies_this_frame = 1;
			} else if (dst.custom_resource->copies_this_frame++ >= dst.custom_resource->max_copies_per_frame) {
				COMMAND_LIST_LOG(state, "  max_copies_per_frame exceeded\n");
				Profiling::max_copies_per_frame_exceeded++;
				return;
			}
		}

		dst.custom_resource->OverrideOutOfBandInfo(&format, &stride);
	}

	FillInMissingInfo(src.type, src_resource, src_view, &stride, &offset, &buf_src_size, &format);

	if (options & ResourceCopyOptions::COPY_MASK) {
		RecreateCompatibleResource(&ini_line, &dst, src_resource,
			pp_cached_resource, p_resource_pool, src_view, pp_cached_view,
			state, mHackerDevice->mStereoHandle,
			options, stride, offset, format, &buf_dst_size);

		if (!*pp_cached_resource) {
			COMMAND_LIST_LOG(state, "  error creating/updating destination resource\n");
			goto out_release;
		}
		dst_resource = *pp_cached_resource;
		if (pp_cached_device)
			*pp_cached_device = state->mOrigDevice1;
		dst_view = *pp_cached_view;

		if (options & ResourceCopyOptions::COPY_DESC) {
			// RecreateCompatibleResource has already done the work
			COMMAND_LIST_LOG(state, "  copying resource description\n");
		} else if (options & ResourceCopyOptions::STEREO2MONO) {
			COMMAND_LIST_LOG(state, "  performing reverse stereo blit\n");
			Profiling::stereo2mono_copies++;

			// TODO: Resolve MSAA to an intermediate resource first
			// if necessary (but keep in mind this may have
			// compatibility issues without a fallback path)

			// The reverse stereo blit seems to only work if the
			// destination resource is stereo. This is a bit
			// bizzare since the whole point of it is to create a
			// double width mono resource, but there you go.
			// We use a second intermediate resource that is forced
			// to stereo and the final destination is forced to
			// mono - once we have done the reverse blit we use an
			// ordinary copy to the final mono resource.

			RecreateCompatibleResource(&(ini_line + L" (intermediate)"),
				NULL, src_resource, &stereo2mono_intermediate,
				p_resource_pool, NULL, NULL,
				state, mHackerDevice->mStereoHandle,
				(ResourceCopyOptions)(options | ResourceCopyOptions::STEREO),
				stride, offset, format, NULL);

			ReverseStereoBlit(stereo2mono_intermediate, src_resource, state);

			mOrigContext1->CopyResource(dst_resource, stereo2mono_intermediate);
		} else if (options & ResourceCopyOptions::RESOLVE_MSAA) {
			COMMAND_LIST_LOG(state, "  resolving MSAA\n");
			Profiling::msaa_resolutions++;
			ResolveMSAA(dst_resource, src_resource, state);
		} else if (buf_dst_size) {
			COMMAND_LIST_LOG(state, "  performing region copy\n");
			Profiling::buffer_region_copies++;
			SpecialCopyBufferRegion(dst_resource, src_resource,
					state, stride, &offset,
					buf_src_size, buf_dst_size);
		} else {
			COMMAND_LIST_LOG(state, "  performing full copy\n");
			Profiling::resource_full_copies++;
			mOrigContext1->CopyResource(dst_resource, src_resource);
		}
	} else {
		COMMAND_LIST_LOG(state, "  copying by reference\n");
		Profiling::resource_reference_copies++;
		dst_resource = src_resource;
		if (src_view && (EquivTarget(src.type) == EquivTarget(dst.type))) {
			dst_view = src_view;
		} else if (*pp_cached_view) {
			if (ViewMatchesResource(*pp_cached_view, dst_resource)) {
				dst_view = *pp_cached_view;
			} else {
				LogDebug("Resource copying: Releasing stale view cache\n");
				(*pp_cached_view)->Release();
				*pp_cached_view = NULL;
			}
		}
		// TODO: If we are referencing to/from a custom resource we
		// currently don't reference the view, but we could so long as
		// the bind flags from the original source are compatible with
		// the bind flags in the final destination. If we implement
		// this, go read the note in CustomResource::Substantiate()
	}

	if (!dst_view) {
		dst_view = CreateCompatibleView(&dst, dst_resource, state,
				stride, offset, format, buf_src_size, options);
		// Not checking for NULL return as view's are not applicable to
		// all types. Legitimate failures are logged.
		*pp_cached_view = dst_view;
	}

	dst.SetResource(state, dst_resource, dst_view, stride, offset, format, buf_dst_size);

	if (options & ResourceCopyOptions::SET_VIEWPORT)
		SetViewportFromResource(state, dst_resource);

out_release:

	if ((options & ResourceCopyOptions::NO_VIEW_CACHE || src.forbid_view_cache)
			&& *pp_cached_view)
	{
		(*pp_cached_view)->Release();
		*pp_cached_view = NULL;
	}

	if (src_view)
		src_view->Release();

	if (src_resource)
		src_resource->Release();
}
