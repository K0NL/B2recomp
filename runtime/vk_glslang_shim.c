/* ssbu_glslang.dll - GLSL (Vulkan dialect) -> SPIR-V through the glslang library, behind a C function.
 *
 * Why a DLL: the glslang 16.6.0 release libraries (reference\upstream\glslang-16.6.0\lib) are MSVC C++ objects built
 * against the DLL C runtime (/MD); the game exe is C, linked by clang. The DLL keeps glslang and its runtime on its own
 * side; the exe loads it with LoadLibrary (plan\phases\V1_vulkan.md, section 4). Build: build_glslang.bat (MSVC).
 *
 * stage: 0 vertex, 1 fragment, 5 compute (the MX_* numbers of mx_translate.h).
 * Returns 1 and a malloc'ed word array (free it with ssbu_spirv_free), or 0 with the compiler's messages in log.
 * Thread-safe: glslang's process initialisation is done once, shaders and programs are per call. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID arg, PVOID *ctx) { (void)once; (void)arg; (void)ctx; return glslang_initialize_process() != 0; }

__declspec(dllexport) void ssbu_spirv_free(uint32_t *words) { free(words); }

__declspec(dllexport) int ssbu_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size) {
    glslang_input_t in;
    int ok = 0;
    if (log && log_size) log[0] = 0;
    *words = NULL; *count = 0;
    if (!InitOnceExecuteOnce(&g_once, init_once, NULL, NULL)) { if (log) snprintf(log, log_size, "glslang_initialize_process failed"); return 0; }
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage == 0 ? GLSLANG_STAGE_VERTEX : stage == 1 ? GLSLANG_STAGE_FRAGMENT : stage == 5 ? GLSLANG_STAGE_COMPUTE : GLSLANG_STAGE_COUNT;
    if (in.stage == GLSLANG_STAGE_COUNT) { if (log) snprintf(log, log_size, "stage %d is not vertex (0), fragment (1) or compute (5)", stage); return 0; }
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = src;
    in.default_version = 460;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT | GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *sh = glslang_shader_create(&in);
    glslang_program_t *pr = NULL;
    if (!sh) { if (log) snprintf(log, log_size, "glslang_shader_create failed"); return 0; }
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        if (log) snprintf(log, log_size, "%s%s", glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
        goto done;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, in.messages)) {
        if (log) snprintf(log, log_size, "%s%s", glslang_program_get_info_log(pr), glslang_program_get_info_debug_log(pr));
        goto done;
    }
    glslang_program_SPIRV_generate(pr, in.stage);
    {
        size_t n = glslang_program_SPIRV_get_size(pr);
        const char *msg = glslang_program_SPIRV_get_messages(pr);
        if (msg && msg[0] && log) snprintf(log, log_size, "%s", msg);
        if (n) {
            *words = malloc(n * sizeof(uint32_t));
            if (*words) { glslang_program_SPIRV_get(pr, *words); *count = n; ok = 1; }
        }
    }
done:
    if (pr) glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return ok;
}
