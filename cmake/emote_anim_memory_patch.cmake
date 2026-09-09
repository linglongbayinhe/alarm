# Local fix for esp_emote_gfx 3.0.5. Never edit managed_components: generate
# a patched translation unit in the build tree and compile it on the same target.
include_guard(GLOBAL)

function(_blufi_emote_replace_once source_var old_text new_text description)
    string(FIND "${${source_var}}" "${old_text}" match_position)
    if(match_position EQUAL -1)
        message(FATAL_ERROR "Emote memory patch: missing ${description}; expected esp_emote_gfx 3.0.5")
    endif()
    string(LENGTH "${old_text}" match_length)
    math(EXPR tail_position "${match_position} + ${match_length}")
    string(SUBSTRING "${${source_var}}" ${tail_position} -1 tail)
    string(FIND "${tail}" "${old_text}" duplicate_position)
    if(NOT duplicate_position EQUAL -1)
        message(FATAL_ERROR "Emote memory patch: ambiguous ${description}; expected exactly one match")
    endif()
    string(REPLACE "${old_text}" "${new_text}" patched "${${source_var}}")
    set(${source_var} "${patched}" PARENT_SCOPE)
endfunction()

function(blufi_generate_emote_anim_memory_patch input_file output_file)
    file(READ "${input_file}" source)
    string(REPLACE "\r\n" "\n" source "${source}")

    # Both allocation routes remain compatible with gfx_anim_reset_frame/free.
    # Alignment must be preserved on the internal fallback as well.
    set(allocator [=[
/* Blufi: decoding is CPU-only; keep transient frames out of scarce internal RAM. */
static void *blufi_anim_alloc(size_t bytes, size_t alignment, const char *purpose)
{
    const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void *buffer = alignment ? heap_caps_aligned_alloc(alignment, bytes, external_caps)
                             : heap_caps_malloc(bytes, external_caps);
    if (buffer == NULL) {
        buffer = alignment ? heap_caps_aligned_alloc(alignment, bytes, internal_caps)
                           : heap_caps_malloc(bytes, internal_caps);
    }
    if (buffer == NULL) {
        ESP_LOGE("anim",
                 "%s allocation failed: bytes=%zu align=%zu "
                 "internal_free=%zu internal_largest=%zu "
                 "psram_free=%zu psram_largest=%zu",
                 purpose, bytes, alignment,
                 heap_caps_get_free_size(internal_caps),
                 heap_caps_get_largest_free_block(internal_caps),
                 heap_caps_get_free_size(external_caps),
                 heap_caps_get_largest_free_block(external_caps));
    }
    return buffer;
}

]=])
    set(palette_function "static esp_err_t gfx_anim_init_palette_cache(gfx_obj_t *obj, gfx_anim_t *anim)\n{")
    _blufi_emote_replace_once(source "${palette_function}"
        "${allocator}${palette_function}" "palette function definition")
    _blufi_emote_replace_once(source
        "anim->frame.color_palette = heap_caps_malloc(palette_size * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);"
        "anim->frame.color_palette = blufi_anim_alloc((size_t)palette_size * sizeof(uint32_t), 0, \"color palette\");"
        "palette allocation")
    _blufi_emote_replace_once(source
        "anim->frame.block_offsets = malloc(anim->frame.desc.blocks * sizeof(uint32_t));"
        "anim->frame.block_offsets = blufi_anim_alloc((size_t)anim->frame.desc.blocks * sizeof(uint32_t), 0, \"block offsets\");"
        "block offsets allocation")
    _blufi_emote_replace_once(source
        "anim->frame.pixel_buffer = heap_caps_aligned_alloc(16, pixel_buffer_size, MALLOC_CAP_DEFAULT);"
        "anim->frame.pixel_buffer = blufi_anim_alloc(pixel_buffer_size, 16, \"24-bit pixel buffer\");"
        "aligned pixel allocation")
    _blufi_emote_replace_once(source
        "anim->frame.pixel_buffer = malloc(pixel_buffer_size);"
        "anim->frame.pixel_buffer = blufi_anim_alloc(pixel_buffer_size, 0, \"indexed pixel buffer\");"
        "indexed pixel allocation")

    get_filename_component(output_dir "${output_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    # Avoid updating timestamps and recompiling on every configure.
    if(EXISTS "${output_file}")
        file(READ "${output_file}" existing)
        if(existing STREQUAL source)
            return()
        endif()
    endif()
    file(WRITE "${output_file}" "${source}")
endfunction()

function(blufi_apply_emote_anim_memory_patch target)
    idf_component_get_property(component_dir espressif2022__esp_emote_gfx COMPONENT_DIR)
    set(original "${component_dir}/src/widget/anim/gfx_anim.c")
    set(patched "${CMAKE_BINARY_DIR}/blufi_patches/esp_emote_gfx/gfx_anim.c")
    get_target_property(sources ${target} SOURCES)
    get_target_property(target_source_dir ${target} SOURCE_DIR)
    set(replacement_sources)
    set(match_count 0)
    get_filename_component(original_absolute "${original}" ABSOLUTE)
    foreach(source IN LISTS sources)
        get_filename_component(source_absolute "${source}" ABSOLUTE BASE_DIR "${target_source_dir}")
        if(source_absolute STREQUAL original_absolute)
            list(APPEND replacement_sources "${patched}")
            math(EXPR match_count "${match_count} + 1")
        else()
            list(APPEND replacement_sources "${source}")
        endif()
    endforeach()
    if(NOT match_count EQUAL 1)
        message(FATAL_ERROR "Emote memory patch: expected gfx_anim.c once in ${target}, found ${match_count}")
    endif()

    blufi_generate_emote_anim_memory_patch("${original}" "${patched}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    set_property(TARGET ${target} PROPERTY SOURCES "${replacement_sources}")
    message(STATUS "Blufi Emote: PSRAM-first animation memory patch enabled (esp_emote_gfx 3.0.5)")
endfunction()
