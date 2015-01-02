/*
 * Centaurean Density
 *
 * Copyright (c) 2013, Guillaume Voirin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Centaurean nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * 07/12/13 15:49
 *
 * -----------------
 * Mandala algorithm
 * -----------------
 *
 * Author(s)
 * Guillaume Voirin (https://github.com/gpnuma)
 * Piotr Tarsa (https://github.com/tarsa)
 *
 * Description
 * Very fast two level dictionary hash algorithm derived from Chameleon, with predictions lookup
 */

#include "kernel_mandala_decode.h"

DENSITY_FORCE_INLINE DENSITY_KERNEL_DECODE_STATE density_mandala_decode_check_state(density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    if (out->available_bytes < DENSITY_MANDALA_DECODE_MINIMUM_OUTPUT_LOOKAHEAD)
        return DENSITY_KERNEL_DECODE_STATE_STALL_ON_OUTPUT_BUFFER;

    switch (state->signaturesCount) {
        case DENSITY_MANDALA_PREFERRED_EFFICIENCY_CHECK_SIGNATURES:
            if (state->efficiencyChecked ^ 0x1) {
                state->efficiencyChecked = 1;
                return DENSITY_KERNEL_DECODE_STATE_INFO_EFFICIENCY_CHECK;
            }
            break;
        case DENSITY_MANDALA_PREFERRED_BLOCK_SIGNATURES:
            state->signaturesCount = 0;
            state->efficiencyChecked = 0;

            if (state->resetCycle)
                state->resetCycle--;
            else {
                density_byte resetDictionaryCycleShift = state->parameters.as_bytes[0];
                if (resetDictionaryCycleShift) {
                    density_mandala_dictionary_reset(&state->dictionary);
                    state->resetCycle = (uint_fast64_t) (1 << resetDictionaryCycleShift) - 1;
                }
            }

            return DENSITY_KERNEL_DECODE_STATE_INFO_NEW_BLOCK;
        default:
            break;
    }
    return DENSITY_KERNEL_DECODE_STATE_READY;
}

DENSITY_FORCE_INLINE void density_mandala_decode_read_signature(density_memory_location *restrict in, density_mandala_decode_state *restrict state) {
    state->signature = DENSITY_LITTLE_ENDIAN_64(*(density_mandala_signature *) (in->pointer));
    in->pointer += sizeof(density_mandala_signature);
    in->available_bytes -= sizeof(density_mandala_signature);
    state->shift = 0;
    state->signaturesCount++;
}

DENSITY_FORCE_INLINE void density_mandala_decode_read_compressed_chunk(uint32_t *restrict hash, density_memory_location *restrict in) {
    *hash = *(uint16_t *) (in->pointer);
    in->pointer += sizeof(uint16_t);
    in->available_bytes -= sizeof(uint16_t);
}

DENSITY_FORCE_INLINE void density_mandala_decode_read_uncompressed_chunk(uint32_t *restrict chunk, density_memory_location *restrict in) {
    *chunk = *(uint32_t *) (in->pointer);
    in->pointer += sizeof(uint32_t);
    in->available_bytes -= sizeof(uint32_t);
}

DENSITY_FORCE_INLINE void density_mandala_decode_predicted_chunk(uint32_t *restrict hash, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    uint32_t chunk = (&state->dictionary.prediction_entries[state->lastHash])->next_chunk_prediction;
    DENSITY_MANDALA_HASH_ALGORITHM(*hash, DENSITY_LITTLE_ENDIAN_32(chunk));

    *(uint32_t *) (out->pointer) = chunk;
    out->pointer += sizeof(uint32_t);
    out->available_bytes -= sizeof(uint32_t);
}

DENSITY_FORCE_INLINE void density_mandala_write_decompressed_chunk(const uint32_t *restrict chunk, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    *(uint32_t *) (out->pointer) = *chunk;
    out->pointer += sizeof(uint32_t);
    out->available_bytes -= sizeof(uint32_t);

    (&state->dictionary.prediction_entries[state->lastHash])->next_chunk_prediction = *chunk;
}

DENSITY_FORCE_INLINE void density_mandala_decode_compressed_chunk_a(const uint32_t *restrict hash, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    uint32_t chunk = (&state->dictionary.entries[DENSITY_LITTLE_ENDIAN_16(*hash)])->chunk_a;

    density_mandala_write_decompressed_chunk(&chunk, out, state);
}

DENSITY_FORCE_INLINE void density_mandala_decode_compressed_chunk_b(const uint32_t *restrict hash, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    density_mandala_dictionary_entry *entry = &state->dictionary.entries[DENSITY_LITTLE_ENDIAN_16(*hash)];
    uint32_t swapped_chunk = entry->chunk_b;

    entry->chunk_b = entry->chunk_a;
    entry->chunk_a = swapped_chunk;

    density_mandala_write_decompressed_chunk(&swapped_chunk, out, state);
}

DENSITY_FORCE_INLINE void density_mandala_decode_uncompressed_chunk(uint32_t *restrict hash, const uint32_t *restrict chunk, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    density_mandala_dictionary_entry *entry;

    DENSITY_MANDALA_HASH_ALGORITHM(*hash, DENSITY_LITTLE_ENDIAN_32(*chunk));
    entry = &state->dictionary.entries[*hash];
    entry->chunk_b = entry->chunk_a;
    entry->chunk_a = *chunk;

    density_mandala_write_decompressed_chunk(chunk, out, state);
}

DENSITY_FORCE_INLINE void density_mandala_decode_kernel(density_memory_location *restrict in, density_memory_location *restrict out, const DENSITY_MANDALA_SIGNATURE_FLAG mode, density_mandala_decode_state *restrict state) {
    uint32_t hash = 0;
    uint32_t chunk;

    switch (mode) {
        case DENSITY_MANDALA_SIGNATURE_FLAG_PREDICTED:
            density_mandala_decode_predicted_chunk(&hash, out, state);
            break;
        case DENSITY_MANDALA_SIGNATURE_FLAG_MAP_A:
            density_mandala_decode_read_compressed_chunk(&hash, in);
            density_mandala_decode_compressed_chunk_a(&hash, out, state);
            break;
        case DENSITY_MANDALA_SIGNATURE_FLAG_MAP_B:
            density_mandala_decode_read_compressed_chunk(&hash, in);
            density_mandala_decode_compressed_chunk_b(&hash, out, state);
            break;
        case DENSITY_MANDALA_SIGNATURE_FLAG_CHUNK:
            density_mandala_decode_read_uncompressed_chunk(&chunk, in);
            density_mandala_decode_uncompressed_chunk(&hash, &chunk, out, state);
            break;
    }

    state->lastHash = hash;
}

DENSITY_FORCE_INLINE const DENSITY_MANDALA_SIGNATURE_FLAG density_mandala_decode_get_signature_flag(density_mandala_decode_state *state) {
    return (DENSITY_MANDALA_SIGNATURE_FLAG const) ((state->signature >> state->shift) & 0x3);
}

DENSITY_FORCE_INLINE void density_mandala_decode_process_data_fast(density_memory_location *restrict in, density_memory_location *restrict out, density_mandala_decode_state *restrict state) {
    while (state->shift ^ 64) {
        density_mandala_decode_kernel(in, out, density_mandala_decode_get_signature_flag(state), state);
        state->shift += 2;
    }
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_DECODE_STATE density_mandala_decode_init(density_mandala_decode_state *state, const density_main_header_parameters parameters, const uint_fast32_t endDataOverhead) {
    state->signaturesCount = 0;
    state->efficiencyChecked = 0;
    density_mandala_dictionary_reset(&state->dictionary);

    state->parameters = parameters;
    density_byte resetDictionaryCycleShift = state->parameters.as_bytes[0];
    if (resetDictionaryCycleShift)
        state->resetCycle = (uint_fast64_t) (1 << resetDictionaryCycleShift) - 1;

    state->endDataOverhead = endDataOverhead;

    state->signatureBytes = 0;
    state->uncompressedChunkBytes = 0;

    state->process = DENSITY_MANDALA_DECODE_PROCESS_SIGNATURES_AND_DATA_FAST;

    return DENSITY_KERNEL_DECODE_STATE_READY;
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_DECODE_STATE density_mandala_decode_process(density_memory_teleport *restrict in, density_memory_location *restrict out, density_mandala_decode_state *restrict state, const density_bool flush) {
    /*DENSITY_KERNEL_DECODE_STATE returnState;
    density_memory_location *readMemoryLocation;

    switch (state->process) {
        case DENSITY_MANDALA_DECODE_PROCESS_PREPARE_NEW_BLOCK:
        prepare_new_block:
            if ((returnState = density_mandala_decode_check_state(out, state)))
                return returnState;
            state->process = DENSITY_MANDALA_DECODE_PROCESS_SIGNATURE;

        case DENSITY_MANDALA_DECODE_PROCESS_SIGNATURE:
            if (flush) {
                uint_fast64_t remaining = density_memory_teleport_available(in) - sizeof(density_block_footer) - sizeof(density_main_footer);
                if (remaining < DENSITY_MANDALA_DECODE_PROCESS_UNIT_SIZE) {
                    density_memory_teleport_copy(in, out, remaining);
                    return DENSITY_KERNEL_DECODE_STATE_FINISHED;
                }
            }
            if (!(readMemoryLocation = density_memory_teleport_read(in, sizeof(density_mandala_signature))))
                return DENSITY_KERNEL_DECODE_STATE_STALL_ON_INPUT_BUFFER;
            density_mandala_decode_read_signature(readMemoryLocation, state);
            readMemoryLocation->available_bytes -= sizeof(density_mandala_signature);
            state->bodyLength = (uint_fast32_t) (sizeof(uint32_t) * bitsizeof(density_chameleon_signature) - __builtin_popcountll(state->signature) * (sizeof(uint32_t) - sizeof(uint16_t)));
            state->process = DENSITY_CHAMELEON_DECODE_PROCESS_DECOMPRESS_BODY;

        case DENSITY_CHAMELEON_DECODE_PROCESS_DECOMPRESS_BODY:
            if (!(readMemoryLocation = density_memory_teleport_read(in, state->bodyLength)))
                return DENSITY_KERNEL_DECODE_STATE_STALL_ON_INPUT_BUFFER;
            density_chameleon_decode_process_data(readMemoryLocation, out, state);
            readMemoryLocation->available_bytes -= state->bodyLength;
            out->available_bytes -= bitsizeof(density_chameleon_signature) * sizeof(uint32_t);
            state->process = DENSITY_CHAMELEON_DECODE_PROCESS_PREPARE_NEW_BLOCK;
            goto prepare_new_block;
    }*/

    return DENSITY_KERNEL_DECODE_STATE_READY;
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_DECODE_STATE density_mandala_decode_finish(density_mandala_decode_state *state) {
    return DENSITY_KERNEL_DECODE_STATE_READY;
}