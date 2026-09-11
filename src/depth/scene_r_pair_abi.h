// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <windows.h>
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef struct ID3D11ShaderResourceView ID3D11ShaderResourceView;

#define ETS2_SCENE_R_PAIR_ABI_VERSION 1u

// ABI V1 is for the x64 addon pair. Runtime/source addresses are opaque receipt
// identities, not borrowed COM references and not values to dereference.
typedef struct ETS2_SceneRRequestV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t runtime;
    ID3D11DeviceContext* context;
    ID3D11Device* device;
    ID3D11Texture2D* final_sbs;
} ETS2_SceneRRequestV1;

typedef struct ETS2_SceneREyeV1 {
    uint64_t source_color;
    uint64_t resolved_color;
    uint64_t resolve_sequence;
    uint64_t color_generation;
    uint64_t depth_generation;
    uint32_t candidate_index;
    uint32_t reserved;
    ID3D11ShaderResourceView* color;
    ID3D11ShaderResourceView* depth;
} ETS2_SceneREyeV1;

typedef struct ETS2_SceneRPairV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t epoch;
    uint64_t generation;
    uint64_t runtime_generation;
    uint32_t scene_width;
    uint32_t scene_height;
    uint32_t final_eye_width;
    uint32_t final_height;
    ETS2_SceneREyeV1 eye[2];
} ETS2_SceneRPairV1;

// out must point to a writable, complete V1 structure. It must not already own
// references: release any previous successful result first. Success transfers
// one AddRef for every eye[].color/depth; caller Release()s all four. Failure
// zeroes the complete output. No callback, GPU completion or content guarantee
// is implied by reference ownership. The producer promises immutable copies.
typedef HRESULT (WINAPI* ETS2_AcquireSceneRPairV1Fn)(
    const ETS2_SceneRRequestV1* request, ETS2_SceneRPairV1* out);
