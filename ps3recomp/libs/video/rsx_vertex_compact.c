#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

static inline u32 rsx_compact_be_u32(const u8* p)
{
#if defined(__GNUC__) || defined(__clang__)
    u32 raw;
    memcpy(&raw, p, 4);
    return __builtin_bswap32(raw);
#else
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
#endif
}

static inline u16 rsx_compact_be_u16(const u8* p)
{
#if defined(__GNUC__) || defined(__clang__)
    u16 raw;
    memcpy(&raw, p, 2);
    return __builtin_bswap16(raw);
#else
    return (u16)((p[0] << 8) | p[1]);
#endif
}

static inline float rsx_compact_be_f32(const u8* p)
{
    const u32 w = rsx_compact_be_u32(p);
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static inline float rsx_compact_be_f16(const u8* p)
{
    const u16 h = rsx_compact_be_u16(p);
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
    __fp16 hf;
    memcpy(&hf, &h, sizeof(hf));
    return (float)hf;
#else
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
#endif
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = (float)source[component] * (1.0f / 255.0f);
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)rsx_compact_be_u16(source + component * 2);
            out[component] = (float)value * (1.0f / 32767.0f);
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)rsx_compact_be_u16(source + component * 2);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word = rsx_compact_be_u32(source);
            const s32 x = ((s32)(word << 21)) >> 21;
            const s32 y = ((s32)(word << 10)) >> 21;
            const s32 z = ((s32)word) >> 22;
            out[0] = (float)x * (1.0f / 1023.0f);
            out[1] = (float)y * (1.0f / 1023.0f);
            out[2] = (float)z * (1.0f / 511.0f);
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_default(const rsx_dispatch* rsx, u32 attr, float out[4])
{
    rsx_dsp_vertex_default(rsx, attr, out);
    if (attr != 3)
        return;
    const u32 base = (0x1C00u + attr * 0x10u) >> 2;
    const u32 written = rsx->seen[base] | rsx->seen[base + 1] |
        rsx->seen[base + 2] | rsx->seen[base + 3];
    /* Preserve the legacy white color fallback only for an unwritten
     * attribute. An explicit (0,0,0,1) is shader input, even when it equals
     * the hardware default; a shader may swizzle x into output alpha. */
    if (!written && out[0] == 0.0f && out[1] == 0.0f &&
        out[2] == 0.0f && out[3] == 1.0f)
        out[0] = out[1] = out[2] = 1.0f;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    plan->inline_stride = 0;
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 attr = layout->attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        fetch->all_in_range = 0;
        rsx_vertex_fetch_default(rsx, attr, fetch->default_value);
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (rsx_vertex_freq_plain) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        fetch->all_in_range = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        const int is_plain = rsx_vertex_freq_plain(fetch->desc.frequency,
                                                   (plan->divider_mask >> attr) & 1u);
        if (is_plain) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
        if (is_plain) fetch->all_in_range = 1;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            rsx_dsp_resolve_vertex_offset(plan->base_offset,
                                          fetch->desc.offset) +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

/* The stable-range loop for one attribute and one vertex type: `decode`
 * reads `src` into `v` (pre-set to 0,0,0,1) for components c < n. Elements
 * outside the stable range take the one-vertex path. */
#define RSX_FETCH_LOOP(decode)                                                  \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                           \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;\
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u32 type = fetch->desc.type;
        u8* dst_base = out + slot * 16u;
        if (!type || !fetch->desc.size) {
            /* Disabled attribute: broadcast constant default_value once per vertex. */
            const float d0 = fetch->default_value[0], d1 = fetch->default_value[1];
            const float d2 = fetch->default_value[2], d3 = fetch->default_value[3];
            u8* dst = dst_base;
            for (u32 i = 0; i < count; i++, dst += stride) {
                float* v = (float*)dst;
                v[0] = d0; v[1] = d1; v[2] = d2; v[3] = d3;
            }
            continue;
        }
        const u8* base = fetch->stable_base;
        if (!base || type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(dst_base + (size_t)i * stride)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;

        /* Bounds-hoisted fast path: plain per-vertex attributes where
         * stable_base already covers [plain_first..plain_last] (99%+ of draws). */
        if (fetch->all_in_range && plain) {
            u8* dst = dst_base;
            switch (type) {
            case RSX_VTX_TYPE_FLOAT:
                if (n == 4) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
#ifdef __ARM_NEON
                        vst1q_u8(dst, vrev32q_u8(vld1q_u8(src)));
#else
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f32(src + 0);
                        v[1] = rsx_compact_be_f32(src + 4);
                        v[2] = rsx_compact_be_f32(src + 8);
                        v[3] = rsx_compact_be_f32(src + 12);
#endif
                    }
                    continue;
                } else if (n == 3) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f32(src + 0);
                        v[1] = rsx_compact_be_f32(src + 4);
                        v[2] = rsx_compact_be_f32(src + 8);
                        v[3] = 1.0f;
                    }
                    continue;
                } else if (n == 2) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f32(src + 0);
                        v[1] = rsx_compact_be_f32(src + 4);
                        v[2] = 0.0f;
                        v[3] = 1.0f;
                    }
                    continue;
                }
                break;
            case RSX_VTX_TYPE_HALF:
                if (n == 2) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f16(src + 0);
                        v[1] = rsx_compact_be_f16(src + 2);
                        v[2] = 0.0f;
                        v[3] = 1.0f;
                    }
                    continue;
                } else if (n == 4) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
#if defined(__aarch64__) && defined(__ARM_NEON)
                        uint8x8_t raw = vld1_u8(src);
                        float16x4_t h4 = vreinterpret_f16_u8(vrev16_u8(raw));
                        vst1q_f32((float*)dst, vcvt_f32_f16(h4));
#else
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f16(src + 0);
                        v[1] = rsx_compact_be_f16(src + 2);
                        v[2] = rsx_compact_be_f16(src + 4);
                        v[3] = rsx_compact_be_f16(src + 6);
#endif
                    }
                    continue;
                } else if (n == 3) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = rsx_compact_be_f16(src + 0);
                        v[1] = rsx_compact_be_f16(src + 2);
                        v[2] = rsx_compact_be_f16(src + 4);
                        v[3] = 1.0f;
                    }
                    continue;
                }
                break;
            case RSX_VTX_TYPE_UNORM8:
                if (n == 4) {
                    const float inv255 = 1.0f / 255.0f;
#if defined(__aarch64__) && defined(__ARM_NEON)
                    const float32x4_t vinv255 = vdupq_n_f32(inv255);
#endif
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
#if defined(__aarch64__) && defined(__ARM_NEON)
                        u32 raw32;
                        memcpy(&raw32, src, 4);
                        uint8x8_t b8 = vreinterpret_u8_u32(vdup_n_u32(raw32));
                        uint16x4_t u16v = vget_low_u16(vmovl_u8(b8));
                        float32x4_t f4 = vmulq_f32(vcvtq_f32_u32(vmovl_u16(u16v)), vinv255);
                        vst1q_f32((float*)dst, f4);
#else
                        float* v = (float*)dst;
                        v[0] = (float)src[0] * inv255;
                        v[1] = (float)src[1] * inv255;
                        v[2] = (float)src[2] * inv255;
                        v[3] = (float)src[3] * inv255;
#endif
                    }
                    continue;
                }
                break;
            case RSX_VTX_TYPE_UINT8:
                if (n == 4) {
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
#if defined(__aarch64__) && defined(__ARM_NEON)
                        u32 raw32;
                        memcpy(&raw32, src, 4);
                        uint8x8_t b8 = vreinterpret_u8_u32(vdup_n_u32(raw32));
                        uint16x4_t u16v = vget_low_u16(vmovl_u8(b8));
                        vst1q_f32((float*)dst, vcvtq_f32_u32(vmovl_u16(u16v)));
#else
                        float* v = (float*)dst;
                        v[0] = (float)src[0];
                        v[1] = (float)src[1];
                        v[2] = (float)src[2];
                        v[3] = (float)src[3];
#endif
                    }
                    continue;
                }
                break;
            case RSX_VTX_TYPE_SNORM16:
                if (n == 2) {
                    const float inv32767 = 1.0f / 32767.0f;
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = (float)(s16)rsx_compact_be_u16(src + 0) * inv32767;
                        v[1] = (float)(s16)rsx_compact_be_u16(src + 2) * inv32767;
                        v[2] = 0.0f;
                        v[3] = 1.0f;
                    }
                    continue;
                } else if (n == 3) {
                    const float inv32767 = 1.0f / 32767.0f;
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
                        float* v = (float*)dst;
                        v[0] = (float)(s16)rsx_compact_be_u16(src + 0) * inv32767;
                        v[1] = (float)(s16)rsx_compact_be_u16(src + 2) * inv32767;
                        v[2] = (float)(s16)rsx_compact_be_u16(src + 4) * inv32767;
                        v[3] = 1.0f;
                    }
                    continue;
                } else if (n == 4) {
                    const float inv32767 = 1.0f / 32767.0f;
#if defined(__aarch64__) && defined(__ARM_NEON)
                    const float32x4_t vinv = vdupq_n_f32(inv32767);
#endif
                    for (u32 i = 0; i < count; i++, dst += stride) {
                        const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                        const u8* src = base + (size_t)(el - first) * fstride;
#if defined(__aarch64__) && defined(__ARM_NEON)
                        int16x4_t s16v = vreinterpret_s16_u8(vrev16_u8(vld1_u8(src)));
                        vst1q_f32((float*)dst, vmulq_f32(vcvtq_f32_s32(vmovl_s16(s16v)), vinv));
#else
                        float* v = (float*)dst;
                        v[0] = (float)(s16)rsx_compact_be_u16(src + 0) * inv32767;
                        v[1] = (float)(s16)rsx_compact_be_u16(src + 2) * inv32767;
                        v[2] = (float)(s16)rsx_compact_be_u16(src + 4) * inv32767;
                        v[3] = (float)(s16)rsx_compact_be_u16(src + 6) * inv32767;
#endif
                    }
                    continue;
                }
                break;
            case RSX_VTX_TYPE_CMP32: {
                const float inv1023 = 1.0f / 1023.0f, inv511 = 1.0f / 511.0f;
                for (u32 i = 0; i < count; i++, dst += stride) {
                    const u32 el = (refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu;
                    const u32 word = rsx_compact_be_u32(base + (size_t)(el - first) * fstride);
                    const s32 x = ((s32)(word << 21)) >> 21;
                    const s32 y = ((s32)(word << 10)) >> 21;
                    const s32 z = ((s32)word) >> 22;
                    float* v = (float*)dst;
                    v[0] = (float)x * inv1023;
                    v[1] = (float)y * inv1023;
                    v[2] = (float)z * inv511;
                    v[3] = 1.0f;
                }
                continue;
            }
            default:
                break;
            }
        }

        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
            break;
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_LOOP(v[c] = (float)src[c] * (1.0f / 255.0f))
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)rsx_compact_be_u16(src + c * 2) * (1.0f / 32767.0f))
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)rsx_compact_be_u16(src + c * 2))
            break;
        case RSX_VTX_TYPE_CMP32:
            for (u32 i = 0; i < count; i++) {
                float* v = (float*)(out + (size_t)i * stride + slot * 16u);
                const u32 element = plain
                    ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)
                    : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,
                                               fetch->desc.frequency, modulo);
                if (element - first > span) {
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;
                    continue;
                }
                const u32 word = rsx_compact_be_u32(base + (u64)(element - first) * fstride);
                const s32 x = ((s32)(word << 21)) >> 21;
                const s32 y = ((s32)(word << 10)) >> 21;
                const s32 z = ((s32)word) >> 22;
                v[0] = (float)x * (1.0f / 1023.0f);
                v[1] = (float)y * (1.0f / 1023.0f);
                v[2] = (float)z * (1.0f / 511.0f);
                v[3] = 1.0f;
            }
            break;
        default:
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    /* Fast direct-indexed O(1) path for single-base_index draws whose vertex
     * IDs span <= 65536 (covers virtually all PS3 16-bit indexed meshes with
     * zero 64-bit hashing and L1-friendly memory access). */
    const u32 base0 = refs[0].base_index;
    u32 min_vid = refs[0].vertex_id, max_vid = refs[0].vertex_id;
    int same_base = 1;
    for (u32 i = 1; i < count; i++) {
        if (refs[i].base_index != base0) { same_base = 0; break; }
        const u32 vid = refs[i].vertex_id;
        if (vid < min_vid) min_vid = vid;
        if (vid > max_vid) max_vid = vid;
    }
    const u32 vid_span = same_base ? (max_vid - min_vid + 1u) : 0u;
    if (same_base && vid_span > 0u && vid_span <= 65536u) {
        if (vid_span > remap->direct_capacity) {
            u32 cap = remap->direct_capacity ? remap->direct_capacity : 1024u;
            while (cap < vid_span) cap *= 2u;
            if (cap > 65536u) cap = 65536u;
            u32* dg = (u32*)realloc(remap->direct_gen, (size_t)cap * sizeof(u32));
            u32* dv = (u32*)realloc(remap->direct_val, (size_t)cap * sizeof(u32));
            if (dg) remap->direct_gen = dg;
            if (dv) remap->direct_val = dv;
            if (dg && dv) {
                memset(remap->direct_gen, 0, (size_t)cap * sizeof(u32));
                remap->direct_capacity = cap;
            }
        }
        if (vid_span <= remap->direct_capacity && remap->direct_gen && remap->direct_val) {
            remap->generation++;
            if (!remap->generation) {
                memset(remap->direct_gen, 0, (size_t)remap->direct_capacity * sizeof(u32));
                if (remap->slots)
                    memset(remap->slots, 0, (size_t)remap->slot_capacity * sizeof(*remap->slots));
                remap->generation = 1u;
            }
            const u32 gen = remap->generation;
            u32* const dg = remap->direct_gen;
            u32* const dv = remap->direct_val;
            u32* const occ = remap->occurrence_to_unique;
            u32 unique = 0;
            for (u32 i = 0; i < count; i++) {
                const u32 idx = refs[i].vertex_id - min_vid;
                if (dg[idx] != gen) {
                    dg[idx] = gen;
                    dv[idx] = unique;
                    refs[unique].vertex_id = refs[i].vertex_id;
                    refs[unique].base_index = base0;
                    occ[i] = unique;
                    unique++;
                } else {
                    occ[i] = dv[idx];
                }
            }
            *unique_count = unique;
            return 1;
        }
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        if (remap->direct_gen)
            memset(remap->direct_gen, 0, (size_t)remap->direct_capacity * sizeof(u32));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            if (remap->direct_gen)
                memset(remap->direct_gen, 0, (size_t)remap->direct_capacity * sizeof(u32));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    free(remap->direct_gen);
    free(remap->direct_val);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
