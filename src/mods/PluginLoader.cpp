#include <filesystem>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "REFramework.hpp"
#include "reframework/API.hpp"
#include "utility/String.hpp"

#include "sdk/ResourceManager.hpp"

#include "APIProxy.hpp"
#include "ScriptRunner.hpp"
#include "PluginLoader.hpp"

REFrameworkPluginVersion g_plugin_version{
    REFRAMEWORK_PLUGIN_VERSION_MAJOR, REFRAMEWORK_PLUGIN_VERSION_MINOR, REFRAMEWORK_PLUGIN_VERSION_PATCH, REFRAMEWORK_GAME_NAME};

namespace reframework {
REFrameworkRendererData g_renderer_data{
    REFRAMEWORK_RENDERER_D3D12, nullptr, nullptr, nullptr
};
}

REFrameworkPluginFunctions g_plugin_functions{
    reframework_on_lua_state_created,
    reframework_on_lua_state_destroyed,
    reframework_on_frame,
    reframework_on_pre_application_entry,
    reframework_on_post_application_entry,
    reframework_lock_lua,
    reframework_unlock_lua,
    reframework_on_device_reset,
    reframework_on_message,
    [](const char* format, ...) { 
        va_list args{};
        va_start(args, format);
        auto str = utility::format_string(format, args);
        va_end(args);
        spdlog::error("[Plugin] {}", str);
    },
    [](const char* format, ...) { 
        va_list args{};
        va_start(args, format);
        auto str = utility::format_string(format, args);
        va_end(args);
        spdlog::warn("[Plugin] {}", str);
    },
    [](const char* format, ...) { 
        va_list args{};
        va_start(args, format);
        auto str = utility::format_string(format, args);
        va_end(args);
        spdlog::info("[Plugin] {}", str);
    },
};

REFrameworkSDKFunctions g_sdk_functions {
    []() -> REFrameworkTDBHandle { return (REFrameworkTDBHandle)sdk::RETypeDB::get(); },
    []() { return (REFrameworkResourceManagerHandle)sdk::ResourceManager::get(); },
    []() { return (REFrameworkVMContextHandle)sdk::get_thread_context(); }, // get_vm_context
    [](const char* name) -> REFrameworkManagedObjectHandle { // typeof
        auto tdef = sdk::RETypeDB::get()->find_type(name);

        if (tdef == nullptr) {
            return nullptr;
        }

        return (REFrameworkManagedObjectHandle)tdef->get_runtime_type();
    },
    [](const char* name) -> REFrameworkManagedObjectHandle {
        return (REFrameworkManagedObjectHandle)g_framework->get_globals()->get(name);
    },
    [](const char* name) {
        return sdk::get_native_singleton(name);
    },
    // get_managed_singletons
    [](REFrameworkManagedSingleton* out, unsigned int out_size, unsigned int* out_count) -> REFrameworkResult {
        auto singletons = g_framework->get_globals()->get_objects();

        if (out_size < singletons.size() * sizeof(REFrameworkManagedSingleton)) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        uint32_t out_written = 0;

        for (auto instance : singletons) {
            if (instance == nullptr) {
                continue;
            }
            
            auto tdef = utility::re_managed_object::get_type_definition(instance);

            out[out_written].instance = (REFrameworkManagedObjectHandle)instance;
            out[out_written].t = (REFrameworkTypeDefinitionHandle)tdef;
            out[out_written].type_info = (REFrameworkTypeInfoHandle)tdef->get_type();
            
            ++out_written;
        }

        if (out_count != nullptr) {
            *out_count = out_written;
        }

        return REFRAMEWORK_ERROR_NONE;
    },
    // get_native_singletons
    [](REFrameworkNativeSingleton* out, unsigned int out_size, unsigned int* out_count) -> REFrameworkResult {
        auto& native_singletons = g_framework->get_globals()->get_native_singleton_types();

        if (out_size < native_singletons.size() * sizeof(REFrameworkNativeSingleton)) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        uint32_t out_written = 0;

        for (auto t : native_singletons) {
            if (t == nullptr) {
                continue;
            }

            auto instance = utility::re_type::get_singleton_instance(t);

            if (instance == nullptr) {
                continue;
            }

            out[out_written].instance = instance;
            out[out_written].t = (REFrameworkTypeDefinitionHandle)utility::re_type::get_type_definition(t);
            out[out_written].type_info = (REFrameworkTypeInfoHandle)t;
            out[out_written].name = t->name;

            ++out_written;
        }

        if (out_count != nullptr) {
            *out_count = out_written;
        }

        return REFRAMEWORK_ERROR_NONE;
    },
    // create_managed_string
    [](const wchar_t* str) -> REFrameworkManagedObjectHandle {
        return (REFrameworkManagedObjectHandle)sdk::VM::create_managed_string(str);
    },
    [](const char* str) -> REFrameworkManagedObjectHandle {
        return (REFrameworkManagedObjectHandle)sdk::VM::create_managed_string(utility::widen(str));
    },
};

#define RETYPEDEF(var) ((sdk::RETypeDefinition*)var)

REFrameworkTDBTypeDefinition g_type_definition_data {
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_index(); }, 
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_size(); }, 
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_valuetype_size(); }, 
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_fqn_hash(); }, 

    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_name(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_namespace(); },
    [](REFrameworkTypeDefinitionHandle tdef, char* out, unsigned int size, unsigned int* out_len) {
        auto full_name = RETYPEDEF(tdef)->get_full_name();

        if (full_name.size() > size) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        memcpy(out, full_name.c_str(), full_name.size());

        if (out_len != nullptr) {
            *out_len = full_name.size();
        }

        return REFRAMEWORK_ERROR_NONE;
    },

    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->has_fieldptr_offset(); }, 
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_fieldptr_offset(); }, 

    [](REFrameworkTypeDefinitionHandle tdef) -> uint32_t { return RETYPEDEF(tdef)->get_methods().size(); },
    [](REFrameworkTypeDefinitionHandle tdef) -> uint32_t { return RETYPEDEF(tdef)->get_fields().size(); },
    [](REFrameworkTypeDefinitionHandle tdef) -> uint32_t { return RETYPEDEF(tdef)->get_properties().size(); },

    [](REFrameworkTypeDefinitionHandle tdef, REFrameworkTypeDefinitionHandle other) { return RETYPEDEF(tdef)->is_a(RETYPEDEF(other)); },
    [](REFrameworkTypeDefinitionHandle tdef, const char* name) { return RETYPEDEF(tdef)->is_a(name); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->is_value_type(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->is_enum(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->is_by_ref(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->is_pointer(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->is_primitive(); },

    [](REFrameworkTypeDefinitionHandle tdef) { return (unsigned int)RETYPEDEF(tdef)->get_vm_obj_type(); },

    [](REFrameworkTypeDefinitionHandle tdef, const char* name) { return (REFrameworkMethodHandle)RETYPEDEF(tdef)->get_method(name); },
    [](REFrameworkTypeDefinitionHandle tdef, const char* name) { return (REFrameworkFieldHandle)RETYPEDEF(tdef)->get_field(name); },
    [](REFrameworkTypeDefinitionHandle tdef, const char* name) { return (REFrameworkPropertyHandle)nullptr; },

    [](REFrameworkTypeDefinitionHandle tdef, REFrameworkMethodHandle* out, unsigned int out_size, unsigned int* out_len) { 
        auto methods = RETYPEDEF(tdef)->get_methods();

        if (methods.size() == 0) {
            return REFRAMEWORK_ERROR_NONE;
        }

        if (methods.size() * sizeof(REFrameworkMethodHandle) > out_size) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        for (auto& method : methods) {
            *out = (REFrameworkMethodHandle)&method;
            out++;
        }

        if (out_len != nullptr) {
            *out_len = methods.size();
        }

        return REFRAMEWORK_ERROR_NONE;
    },
    [](REFrameworkTypeDefinitionHandle tdef, REFrameworkFieldHandle* out, unsigned int out_size, unsigned int* out_len) { 
        auto fields = RETYPEDEF(tdef)->get_fields();

        if (fields.size() == 0) {
            return REFRAMEWORK_ERROR_NONE;
        }

        if (fields.size() * sizeof(REFrameworkFieldHandle) > out_size) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        for (auto field : fields) {
            *out = (REFrameworkFieldHandle)field;
            out++;
        }

        if (out_len != nullptr) {
            *out_len = fields.size();
        }

        return REFRAMEWORK_ERROR_NONE;
    },

    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->get_instance(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return RETYPEDEF(tdef)->create_instance(); },
    [](REFrameworkTypeDefinitionHandle tdef, unsigned int flags) { return (REFrameworkManagedObjectHandle)RETYPEDEF(tdef)->create_instance_full(flags == REFRAMEWORK_CREATE_INSTANCE_FLAGS_SIMPLIFY); },

    [](REFrameworkTypeDefinitionHandle tdef) { return (REFrameworkTypeDefinitionHandle)RETYPEDEF(tdef)->get_parent_type(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return (REFrameworkTypeDefinitionHandle)RETYPEDEF(tdef)->get_declaring_type(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return (REFrameworkTypeDefinitionHandle)RETYPEDEF(tdef)->get_underlying_type(); },
    [](REFrameworkTypeDefinitionHandle tdef) { return (REFrameworkTypeInfoHandle)RETYPEDEF(tdef)->get_type(); }
};

#define REMETHOD(var) ((sdk::REMethodDefinition*)var)

REFrameworkTDBMethod g_tdb_method_data {
    [](REFrameworkMethodHandle method, void* thisptr, void** in_args, unsigned int in_args_size, void* out, unsigned int out_size) {
        if (sizeof(reframework::InvokeRet) > out_size) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        auto m = REMETHOD(method);

        if (m->get_num_params() != in_args_size / sizeof(void*)) {
            return REFRAMEWORK_ERROR_IN_ARGS_SIZE_MISMATCH;
        }

        std::vector<void*> cpp_args{};

        for (auto i = 0; i < in_args_size / sizeof(void*); i++) {
            cpp_args.push_back(in_args[i]);
        }

        auto ret = m->invoke(thisptr, cpp_args);

        memcpy(out, &ret, sizeof(reframework::InvokeRet));

        if (ret.exception_thrown) {
            return REFRAMEWORK_ERROR_EXCEPTION;
        }

        return REFRAMEWORK_ERROR_NONE;
    },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_function(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_name(); },
    [](REFrameworkMethodHandle method) { return (REFrameworkTypeDefinitionHandle)REMETHOD(method)->get_declaring_type(); },
    [](REFrameworkMethodHandle method) { return (REFrameworkTypeDefinitionHandle)REMETHOD(method)->get_return_type(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_num_params(); },
    [](REFrameworkMethodHandle method, REFrameworkMethodParameter* out, unsigned int out_size, unsigned int* out_len) {
        const auto num_params = REMETHOD(method)->get_num_params();
        
        if (REMETHOD(method)->get_num_params() == 0) {
            return REFRAMEWORK_ERROR_NONE;
        }

        if (num_params * sizeof(REFrameworkMethodParameter) > out_size) {
            return REFRAMEWORK_ERROR_OUT_TOO_SMALL;
        }

        const auto param_names = REMETHOD(method)->get_param_names();
        const auto param_types = REMETHOD(method)->get_param_types();

        for (auto i = 0; i < num_params; ++i) {
            out[i].name = param_names[i];
            out[i].t = (REFrameworkTypeDefinitionHandle)param_types[i];
        }

        if (out_len != nullptr) {
            *out_len = num_params;
        }

        return REFRAMEWORK_ERROR_NONE;
    },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_index(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_virtual_index(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->is_static(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_flags(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_impl_flags(); },
    [](REFrameworkMethodHandle method) { return REMETHOD(method)->get_invoke_id(); }
};

#define REFIELD(var) ((sdk::REField*)(var))

REFrameworkTDBField g_tdb_field_data {
    [](REFrameworkFieldHandle field) { return REFIELD(field)->get_name(); },

    [](REFrameworkFieldHandle field) { return (REFrameworkTypeDefinitionHandle)REFIELD(field)->get_declaring_type(); },
    [](REFrameworkFieldHandle field) { return (REFrameworkTypeDefinitionHandle)REFIELD(field)->get_type(); },

    [](REFrameworkFieldHandle field) { return REFIELD(field)->get_offset_from_base(); },
    [](REFrameworkFieldHandle field) { return REFIELD(field)->get_offset_from_fieldptr(); },
    [](REFrameworkFieldHandle field) { return REFIELD(field)->get_flags(); },

    [](REFrameworkFieldHandle field) { return REFIELD(field)->is_static(); },
    [](REFrameworkFieldHandle field) { return REFIELD(field)->is_literal(); },

    [](REFrameworkFieldHandle field) { return REFIELD(field)->get_init_data(); },
    [](REFrameworkFieldHandle field, void* obj, bool is_value_type) { return REFIELD(field)->get_data_raw(obj, is_value_type); },
};

REFrameworkTDBProperty g_tdb_property_data {
    // todo
};

#define RETDB(var) ((sdk::RETypeDB*)var)

REFrameworkTDB g_tdb_data {
    [](REFrameworkTDBHandle tdb) { return RETDB(tdb)->numTypes; },
    [](REFrameworkTDBHandle tdb) { return RETDB(tdb)->numMethods; },
    [](REFrameworkTDBHandle tdb) { return RETDB(tdb)->numFields; },
    [](REFrameworkTDBHandle tdb) { return RETDB(tdb)->numProperties; },
    [](REFrameworkTDBHandle tdb) { return (unsigned int)RETDB(tdb)->numStringPool; },
    [](REFrameworkTDBHandle tdb) { return (unsigned int)RETDB(tdb)->numBytePool; },
    [](REFrameworkTDBHandle tdb) { return (const char*)RETDB(tdb)->stringPool; },
    [](REFrameworkTDBHandle tdb) { return (unsigned char*)RETDB(tdb)->bytePool; },

    [](REFrameworkTDBHandle tdb, unsigned int index) { return (REFrameworkTypeDefinitionHandle)RETDB(tdb)->get_type(index); },
    [](REFrameworkTDBHandle tdb, const char* name) { return (REFrameworkTypeDefinitionHandle)RETDB(tdb)->find_type(name); },
    [](REFrameworkTDBHandle tdb, unsigned int fqn) { return (REFrameworkTypeDefinitionHandle)RETDB(tdb)->find_type_by_fqn(fqn); },
    [](REFrameworkTDBHandle tdb, unsigned int index) { return (REFrameworkMethodHandle)RETDB(tdb)->get_method(index); },
    [](REFrameworkTDBHandle tdb, const char* type_name, const char* method_name) -> REFrameworkMethodHandle { 
        auto t = RETDB(tdb)->find_type(type_name);

        if (t == nullptr) {
            return nullptr;
        }

        return (REFrameworkMethodHandle)t->get_method(method_name);
    },

    [](REFrameworkTDBHandle tdb, unsigned int index) { return (REFrameworkFieldHandle)RETDB(tdb)->get_field(index); },
    [](REFrameworkTDBHandle tdb, const char* type_name, const char* field_name) -> REFrameworkFieldHandle {
        auto t = RETDB(tdb)->find_type(type_name);

        if (t == nullptr) {
            return nullptr;
        }

        return (REFrameworkFieldHandle)t->get_field(field_name);
    },

    [](REFrameworkTDBHandle tdb, unsigned int index) { return (REFrameworkPropertyHandle)RETDB(tdb)->get_property(index); },
};

#define REMANAGEDOBJECT(var) ((::REManagedObject*)var)

REFrameworkManagedObject g_managed_object_data {
    [](REFrameworkManagedObjectHandle obj) { utility::re_managed_object::add_ref(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj) { utility::re_managed_object::release(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj) { return (REFrameworkTypeDefinitionHandle)utility::re_managed_object::get_type_definition(REMANAGEDOBJECT(obj)); },
    [](void* potential_obj) { return utility::re_managed_object::is_managed_object(potential_obj); },
    [](REFrameworkManagedObjectHandle obj) { return REMANAGEDOBJECT(obj)->referenceCount; },
    [](REFrameworkManagedObjectHandle obj) { return utility::re_managed_object::get_size(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj) { return (unsigned int)utility::re_managed_object::get_vm_type(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj) { return (REFrameworkTypeInfoHandle)utility::re_managed_object::get_type(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj) { return (void*)utility::re_managed_object::get_variables(REMANAGEDOBJECT(obj)); },
    [](REFrameworkManagedObjectHandle obj, const char* name) { return (void*)utility::re_managed_object::get_field_desc(REMANAGEDOBJECT(obj), name); },
    [](REFrameworkManagedObjectHandle obj, const char* name) { return (void*)utility::re_managed_object::get_method_desc(REMANAGEDOBJECT(obj), name); },
};

#define RERESOURCEMGR(var) ((sdk::ResourceManager*)var)

REFrameworkResourceManager g_resource_manager_data {
    [](REFrameworkResourceManagerHandle mgr, const char* type_name, const char* name) -> REFrameworkResourceHandle {
        // NOT a type definition.
        auto t = g_framework->get_types()->get(type_name);

        if (t == nullptr) {
            return nullptr;
        }

        return (REFrameworkResourceHandle)RERESOURCEMGR(mgr)->create_resource(t, utility::widen(name).c_str());
    }
};

#define RERESOURCE(var) ((sdk::Resource*)var)

REFrameworkResource g_resource_data {
    [](REFrameworkResourceHandle res) { RERESOURCE(res)->add_ref(); },
    [](REFrameworkResourceHandle res) { RERESOURCE(res)->release(); }
};

#define RETYPEINFO(var) ((::REType*)var)

REFrameworkTypeInfo g_type_info_data {
    [](REFrameworkTypeInfoHandle ti) -> const char* { return RETYPEINFO(ti)->name; },
    [](REFrameworkTypeInfoHandle ti) { return (REFrameworkTypeDefinitionHandle)utility::re_type::get_type_definition(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti) { return utility::re_type::is_clr_type(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti) { return utility::re_type::is_singleton(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti) { return utility::re_type::get_singleton_instance(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti) { return utility::re_type::create_instance(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti) { return (void*)utility::re_type::get_variables(RETYPEINFO(ti)); },
    [](REFrameworkTypeInfoHandle ti, const char* name) { return (void*)utility::re_type::get_field_desc(RETYPEINFO(ti), name); },
    [](REFrameworkTypeInfoHandle ti, const char* name) { return (void*)utility::re_type::get_method_desc(RETYPEINFO(ti), name); }
};

#define VMCONTEXT(var) ((sdk::VMContext*)var)

REFrameworkVMContext g_vm_context_data {
    // has exception
    [](REFrameworkVMContextHandle ctx) { return VMCONTEXT(ctx)->unkPtr->unkPtr != nullptr; },
    [](REFrameworkVMContextHandle ctx) { VMCONTEXT(ctx)->unhandled_exception(); },
    [](REFrameworkVMContextHandle ctx) { VMCONTEXT(ctx)->local_frame_gc(); },
    [](REFrameworkVMContextHandle ctx, int32_t old) { VMCONTEXT(ctx)->cleanup_after_exception(old); },
};

REFrameworkSDKData g_sdk_data {
    &g_sdk_functions,
    &g_tdb_data,
    &g_type_definition_data,
    &g_tdb_method_data,
    &g_tdb_field_data,
    &g_tdb_property_data,
    &g_managed_object_data,
    &g_resource_manager_data,
    &g_resource_data,
    &g_type_info_data,
    &g_vm_context_data
};

REFrameworkPluginInitializeParam g_plugin_initialize_param{
    nullptr, 
    &g_plugin_version, 
    &g_plugin_functions, 
    &reframework::g_renderer_data,
    &g_sdk_data
};

void verify_sdk_pointers() {
    auto verify = [](auto& g) {
        spdlog::info("Verifying...");

        for (auto i = 0; i < sizeof(g) / sizeof(void*); ++i) {
            if (((void**)&g)[i] == nullptr) {
                spdlog::error("SDK pointer is null at index {}", i);
            }
        }
    };

    spdlog::info("Verifying SDK pointers...");

    verify(g_managed_object_data);
    verify(g_sdk_data);
    verify(g_type_definition_data);
    verify(g_tdb_method_data);
    verify(g_tdb_field_data);
    verify(g_tdb_property_data);
    verify(g_tdb_data);
}

std::shared_ptr<PluginLoader> PluginLoader::get() {
    static auto instance = std::make_shared<PluginLoader>();
    return instance;
}

void PluginLoader::early_init() {
    namespace fs = std::filesystem;

    std::scoped_lock _{m_mux};
    std::string module_path{};

    module_path.resize(1024, 0);
    module_path.resize(GetModuleFileName(nullptr, module_path.data(), module_path.size()));
    spdlog::info("[PluginLoader] Module path {}", module_path);

    auto plugin_path = fs::path{module_path}.parent_path() / "reframework" / "plugins";

    spdlog::info("[PluginLoader] Creating directories {}", plugin_path.string());
    fs::create_directories(plugin_path);
    spdlog::info("[PluginLoader] Loading plugins...");

    // Load all dlls in the plugins directory.
    for (auto&& entry : fs::directory_iterator{plugin_path}) {
        auto&& path = entry.path();

        if (path.has_extension() && path.extension() == ".dll") {
            auto module = LoadLibrary(path.string().c_str());

            if (module == nullptr) {
                spdlog::error("[PluginLoader] Failed to load {}", path.string());
                m_plugin_load_errors.emplace(path.stem().string(), "Failed to load");
                continue;
            }

            spdlog::info("[PluginLoader] Loaded {}", path.string());
            m_plugins.emplace(path.stem().string(), module);
        }
    }
}

std::optional<std::string> PluginLoader::on_initialize() {
    std::scoped_lock _{m_mux};

    // Call reframework_plugin_required_version on any dlls that export it.
    g_plugin_initialize_param.reframework_module = g_framework->get_reframework_module();
    reframework::g_renderer_data.renderer_type = (int)g_framework->get_renderer_type();
    
    if (reframework::g_renderer_data.renderer_type == REFRAMEWORK_RENDERER_D3D11) {
        auto& d3d11 = g_framework->get_d3d11_hook();

        reframework::g_renderer_data.device = d3d11->get_device();
        reframework::g_renderer_data.swapchain = d3d11->get_swap_chain();
    } else if (reframework::g_renderer_data.renderer_type == REFRAMEWORK_RENDERER_D3D12) {
        auto& d3d12 = g_framework->get_d3d12_hook();

        reframework::g_renderer_data.device = d3d12->get_device();
        reframework::g_renderer_data.swapchain = d3d12->get_swap_chain();
        reframework::g_renderer_data.command_queue = d3d12->get_command_queue();
    } else {
        spdlog::error("[PluginLoader] Unsupported renderer type {}", reframework::g_renderer_data.renderer_type);
        return "PluginLoader: Unsupported renderer type detected";
    }

    verify_sdk_pointers();

    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        auto name = it->first;
        auto mod = it->second;
        auto required_version_fn = (REFPluginRequiredVersionFn)GetProcAddress(mod, "reframework_plugin_required_version");

        if (required_version_fn == nullptr) {
            ++it;
            continue;
        }

        REFrameworkPluginVersion required_version{};
        required_version_fn(&required_version);

        spdlog::info(
            "[PluginLoader] {} requires version {}.{}.{}", name, required_version.major, required_version.minor, required_version.patch);

        if (required_version.major != g_plugin_version.major) {
            spdlog::error("[PluginLoader] Plugin {} requires a different major version", name);
            m_plugin_load_errors.emplace(name, "Requires a different major version");
            FreeLibrary(mod);
            it = m_plugins.erase(it);
            continue;
        }

        if (required_version.minor > g_plugin_version.minor) {
            spdlog::error("[PluginLoader] Plugin {} requires a newer minor version", name);
            m_plugin_load_errors.emplace(name, "Requires a newer minor version");
            FreeLibrary(mod);
            it = m_plugins.erase(it);
            continue;
        }

        if (required_version.patch > g_plugin_version.patch) {
            spdlog::warn("[PluginLoader] Plugin {} desires a newer patch version", name);
            m_plugin_load_warnings.emplace(name, "Desires a newer patch version");
        }

        if (required_version.game_name != nullptr && std::string_view{required_version.game_name} != g_plugin_version.game_name) {
            spdlog::error("[PluginLoader] Plugin {} is for a different game {}", name, required_version.game_name);
            m_plugin_load_errors.emplace(name, "Is for a different game");
            FreeLibrary(mod);
            it = m_plugins.erase(it);
            continue;
        }

        ++it;
    }

    // Call reframework_plugin_initialize on any dlls that export it.
    for (auto it = m_plugins.begin(); it != m_plugins.end();) {
        auto name = it->first;
        auto mod = it->second;
        auto init_fn = (REFPluginInitializeFn)GetProcAddress(mod, "reframework_plugin_initialize");

        if (init_fn == nullptr) {
            ++it;
            continue;
        }

        spdlog::info("[PluginLoader] Initializing {}...", name);

        if (!init_fn(&g_plugin_initialize_param)) {
            spdlog::error("[PluginLoader] Failed to initialize {}", name);
            m_plugin_load_errors.emplace(name, "Failed to initialize");
            FreeLibrary(mod);
            it = m_plugins.erase(it);
            continue;
        }

        ++it;
    }

    return std::nullopt;
}

void PluginLoader::on_draw_ui() {
    ImGui::SetNextTreeNodeOpen(false, ImGuiCond_Once);

    if (ImGui::CollapsingHeader(get_name().data())) {
        std::scoped_lock _{m_mux};

        if (!m_plugins.empty()) {
            ImGui::Text("Loaded plugins:");

            for (auto&& [name, _] : m_plugins) {
                ImGui::Text(name.c_str());
            }
        } else {
            ImGui::Text("No plugins loaded.");
        }

        if (!m_plugin_load_errors.empty()) {
            ImGui::Spacing();
            ImGui::Text("Errors:");
            for (auto&& [name, error] : m_plugin_load_errors) {
                ImGui::Text("%s - %s", name.c_str(), error.c_str());
            }
        }

        if (!m_plugin_load_warnings.empty()) {
            ImGui::Spacing();
            ImGui::Text("Warnings:");
            for (auto&& [name, warning] : m_plugin_load_warnings) {
                ImGui::Text("%s - %s", name.c_str(), warning.c_str());
            }
        }
    }
}

bool reframework_on_lua_state_created(REFLuaStateCreatedCb cb) {
    if (cb == nullptr) {
        return false;
    }

    return APIProxy::get()->add_on_lua_state_created(cb);
}

bool reframework_on_lua_state_destroyed(REFLuaStateDestroyedCb cb) {
    if (cb == nullptr) {
        return false;
    }


    return APIProxy::get()->add_on_lua_state_destroyed(cb);
}

bool reframework_on_frame(REFOnFrameCb cb) {
    if (cb == nullptr) {
        return false;
    }

    return APIProxy::get()->add_on_frame(cb);
}

bool reframework_on_pre_application_entry(const char* name, REFOnPreApplicationEntryCb cb) {
    if (cb == nullptr || name == nullptr) {
        return false;
    }

    auto cppname = std::string{name};

    if (cppname.empty()) {
        return false;
    }

    return APIProxy::get()->add_on_pre_application_entry(cppname, cb);
}

bool reframework_on_post_application_entry(const char* name, REFOnPostApplicationEntryCb cb) {
    if (cb == nullptr || name == nullptr) {
        return false;
    }

    auto cppname = std::string{name};

    if (cppname.empty()) {
        return false;
    }

    return APIProxy::get()->add_on_post_application_entry(cppname, cb);
}

void reframework_lock_lua() {
    ScriptRunner::get()->lock();
}

void reframework_unlock_lua() {
    ScriptRunner::get()->unlock();
}

bool reframework_on_device_reset(REFOnDeviceResetCb cb) {
    if (cb == nullptr) {
        return false;
    }

    return APIProxy::get()->add_on_device_reset(cb);
}

bool reframework_on_message(REFOnMessageCb cb) {
    if (cb == nullptr) {
        return false;
    }

    return APIProxy::get()->add_on_message(cb);
}
